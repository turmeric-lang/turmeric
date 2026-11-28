/* notebook-cells-cannot-call-inline-c-spices: turi_env_attach_spice and
 * turi_env_attach_spice_for_module -- an embedder (the notebook) runs a
 * spice's compiled code for the calls the tree-walking interpreter cannot
 * make, the way `tur repl` binds the spice it is started in.
 *
 * Builds a fixture spice in a temp dir whose exports cover each binding rule:
 *   add-loop   -- body IS inline C (a loop the interpreter declines)
 *   wrapped    -- a Turmeric wrapper over a PRIVATE inline-C helper; importing
 *                 the module must not replace the compiled export with it
 *   box-new/-get/-free -- a `defopaque` handle API (shimmed as int class)
 *   tagged-id  -- generic only in a phantom index, never instantiated: no
 *                 definition in the image, so it must not have a shim either
 *                 (a shim calling it left the .so undloadable)
 *   pt-sum / pt-make / b3-sum / b3-make -- by-value records as parameter
 *                 and result, small (passed by value) and large (`const T *`):
 *                 packed from the session's record value, rebuilt from the
 *                 compiled result -- each through a private inline-C helper, so
 *                 only the compiled path can answer
 *   answer     -- shadowed by a host native: the bare name keeps the host's,
 *                 the qualified name reaches the export
 *   greet / even-word? -- a :cstr and a :bool return come back as a string
 *                 and a boolean, not as the bare word
 *   dep-twice  -- calls into a :spices dep (afdep/util): the image is built
 *                 from the spice root with the dep linked in, and the dep's
 *                 exports are not the image's
 *   apply-sum / count-if / pf-apply / pf-sample / crv-new + crv-at + crv-free --
 *                 `^fat` callback parameters: a session closure crosses as a
 *                 fat box around the image's `__ffi_cb<k>` trampoline, which
 *                 calls back into the interpreter -- with a float, a bool
 *                 result, a record argument, a record result, and a callback
 *                 the compiled side stores and calls on a later export call
 *                 (plot's renderers)
 *   papply-sum / pcount3 / ppf-apply / ppf-sample / pb3 / fb3 -- PLAIN
 *                 `(fn ...)` parameters.  A scalar signature is a
 *                 `tur_poly_fn_t { env, fn }` in C, which the shim builds
 *                 from the same box (env the box, fn the trampoline); one
 *                 with a record is fat-normalized, the same word as `^fat`.
 *                 pb3 / fb3 pass a wide record, which crosses as a box
 *                 pointer.  Each also calls a private inline-C helper, so
 *                 only the compiled path can answer
 *
 * Needs a `tur` to build the image: TUR_BIN (ctest sets it), else ./build/tur.
 * Linked with ENABLE_EXPORTS (-rdynamic): the image resolves the runtime it
 * shares with the host (tur_string_release, ...) against the executable.
 *
 *   cmake --build build --target tur_embed_attach_spice
 *   TUR_BIN=$PWD/build/tur ./build/tur_embed_attach_spice
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

static void write_file(const char *dir, const char *rel, const char *text) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    FILE *f = fopen(p, "w");
    if (!f) { fprintf(stderr, "FAIL [cannot write %s]\n", p); failures++; return; }
    fputs(text, f);
    fclose(f);
}

static void make_dir(const char *dir, const char *rel) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    mkdir(p, 0755);
}

static bool is_int(TuriValue v, int64_t want) {
    return v.tag == TURI_INT && v.as_int == want;
}

static bool is_float(TuriValue v, double want) {
    if (v.tag != TURI_FLOAT) {
        if (turi_is_error(v)) fprintf(stderr, "  got error: %s\n", turi_error_message(v));
        else fprintf(stderr, "  got tag %d\n", (int)v.tag);
        return false;
    }
    double d = v.as_float - want;
    if (d < 0) d = -d;
    if (d > 1e-9) fprintf(stderr, "  got %.17g, want %.17g\n", v.as_float, want);
    return d <= 1e-9;
}

static TuriValue native_seven(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_int(7);
}

static const char *FIXTURE =
    "(defmodule afx/core\n"
    "  (import afdep/util :refer [twice])\n"
    "  (export add-loop wrapped box-new box-get box-free tagged-id Pt pt-sum answer\n"
    "          greet even-word? dep-twice B3 pt-make b3-sum b3-make\n"
    "          apply-sum count-if Pf pf-apply pf-sample Crv crv-new crv-at crv-free\n"
    "          papply-sum pcount3 ppf-apply ppf-sample pb3 fb3)\n"
    "  (defn apply-sum [^fat f : (fn [float] float) n : int] : float\n"
    "    ```c\n"
    "    double s = 0;\n"
    "    for (int64_t i = 0; i < n; i++) s += TUR_APPLY1_T(double, double, f, (double)i + 0.5);\n"
    "    return s;\n"
    "    ```)\n"
    "  (defn count-if [^fat p : (fn [int] bool) n : int] : int\n"
    "    ```c\n"
    "    int64_t c = 0;\n"
    "    for (int64_t i = 0; i < n; i++) if (TUR_APPLY1_T(int64_t, int64_t, p, i)) c++;\n"
    "    return c;\n"
    "    ```)\n"
    "  (defstruct Pf [x : float y : float])\n"
    "  (defn pf-apply [^fat f : (fn [Pf] float) x : float] : float\n"
    "    (+ (f (Pf x (* x 2.0))) (__triple-f x)))\n"
    "  (defn pf-sample [^fat f : (fn [float] Pf) t : float] : float\n"
    "    (let [p (f t)] (+ (. p x) (+ (. p y) (__triple-f 0.0)))))\n"
    "  (defn __triple-f [x : float] : float\n"
    "    ```c\n"
    "    double r = 0;\n"
    "    for (int i = 0; i < 3; i++) r += x;\n"
    "    return r;\n"
    "    ```)\n"
    "  (defopaque Crv :ptr<void>)\n"
    "  (defn __crv-new-raw [^fat f : (fn [float] float)] : ptr<void>\n"
    "    ```c\n"
    "    int64_t *c = (int64_t *)malloc(sizeof(int64_t));\n"
    "    for (int i = 0; i < 1; i++) c[i] = f;\n"
    "    return (void *)c;\n"
    "    ```)\n"
    "  (defn __crv-at-raw [c : ptr<void> x : float] : float\n"
    "    ```c\n"
    "    int64_t f = 0;\n"
    "    for (int i = 0; i < 1; i++) f = ((int64_t *)c)[i];\n"
    "    return TUR_APPLY1_T(double, double, f, x);\n"
    "    ```)\n"
    "  (defn crv-new [^fat f : (fn [float] float)] : Crv (:: (__crv-new-raw f) Crv))\n"
    "  (defn crv-at [c : Crv x : float] : float (__crv-at-raw (:: c ptr<void>) x))\n"
    "  (defn crv-free [c : Crv] : nil (__box-free-raw (:: c ptr<void>)))\n"
    "  (defn papply-sum [f : (fn [float] float)] : float\n"
    "    (+ (__triple-f 0.0) (+ (f 0.5) (+ (f 1.5) (f 2.5)))))\n"
    "  (defn pcount3 [p : (fn [int] bool)] : int\n"
    "    (+ (__triple-raw 0) (+ (if (p 1) 1 0) (+ (if (p 2) 1 0) (if (p 3) 1 0)))))\n"
    "  (defn ppf-apply [f : (fn [Pf] float) x : float] : float\n"
    "    (+ (__triple-f 0.0) (f (Pf x (* x 2.0)))))\n"
    "  (defn ppf-sample [f : (fn [float] Pf) t : float] : float\n"
    "    (let [p (f t)] (+ (__triple-f (. p x)) (. p y))))\n"
    "  (defn dep-twice [x : int] : int (+ 1 (twice x)))\n"
    "  (defn add-loop [a : int b : int] : int\n"
    "    ```c\n"
    "    int64_t s = a;\n"
    "    for (int64_t i = 0; i < b; i++) s += 1;\n"
    "    return s;\n"
    "    ```)\n"
    "  (defn __triple-raw [x : int] : int\n"
    "    ```c\n"
    "    int64_t r = 0;\n"
    "    for (int i = 0; i < 3; i++) r += x;\n"
    "    return r;\n"
    "    ```)\n"
    "  (defn wrapped [x : int] : int (+ 1 (__triple-raw x)))\n"
    "  (defopaque Box :ptr<void>)\n"
    "  (defn __box-new-raw [v : int] : ptr<void>\n"
    "    ```c\n"
    "    int64_t *b = (int64_t *)malloc(2 * sizeof(int64_t));\n"
    "    for (int i = 0; i < 2; i++) b[i] = v;\n"
    "    return (void *)b;\n"
    "    ```)\n"
    "  (defn __box-get-raw [b : ptr<void>] : int\n"
    "    ```c\n"
    "    int64_t *p = (int64_t *)b;\n"
    "    int64_t s = 0;\n"
    "    for (int i = 0; i < 2; i++) s += p[i];\n"
    "    return s;\n"
    "    ```)\n"
    "  (defn __box-free-raw [b : ptr<void>] : nil\n"
    "    ```c\n"
    "    for (int i = 0; i < 1; i++) free(b);\n"
    "    ```)\n"
    "  (defn box-new [v : int] : Box (:: (__box-new-raw v) Box))\n"
    "  (defn box-get [b : Box] : int (__box-get-raw (:: b ptr<void>)))\n"
    "  (defn box-free [b : Box] : nil (__box-free-raw (:: b ptr<void>)))\n"
    "  (defopaque Tagged [n] :int)\n"
    "  (defn tagged-id [n] [t : (Tagged n)] : (Tagged n) t)\n"
    "  (defstruct Pt [x : int y : int])\n"
    "  (defn pt-sum [p : Pt] : int (+ (__triple-raw (. p x)) (. p y)))\n"
    "  (defn pt-make [x : int y : int] : Pt (Pt (__triple-raw x) y))\n"
    "  (defstruct B3 [a : int b : int c : int])\n"
    "  (defn pb3 [f : (fn [B3] int)] : int (+ (__triple-raw 0) (f (B3 1 20 300))))\n"
    "  (defn fb3 [^fat f : (fn [B3] int)] : int (+ (__triple-raw 0) (f (B3 4 50 600))))\n"
    "  (defn b3-sum [r : B3] : int (+ (__triple-raw (. r a)) (+ (. r b) (. r c))))\n"
    "  (defn b3-make [a : int b : int c : int] : B3 (B3 (__triple-raw a) b c))\n"
    "  (defn answer [] : int 42)\n"
    "  (defn greet [] : cstr\n"
    "    ```c\n"
    "    static const char *const g[] = { \"hello\", \"world\" };\n"
    "    for (int i = 0; i < 1; i++) {}\n"
    "    return g[0];\n"
    "    ```)\n"
    "  (defn even-word? [n : int] : bool\n"
    "    ```c\n"
    "    int64_t m = n;\n"
    "    for (; m >= 2; m -= 2) {}\n"
    "    return m == 0;\n"
    "    ```))\n";

int main(void) {
    turi_init(false);

    const char *tur = getenv("TUR_BIN");
    static char tur_abs[4096];
    if (!tur || !*tur) {
        if (!realpath("build/tur", tur_abs)) {
            fprintf(stderr, "embed-attach-spice: set TUR_BIN or run from the repo root\n");
            return 2;
        }
        tur = tur_abs;
    }

    char root[] = "/tmp/tur-embed-attach-XXXXXX";
    if (!mkdtemp(root)) { CHECK(0, "mkdtemp for the fixture spice"); return 1; }
    make_dir(root, "afx");
    make_dir(root, "afx/src");
    make_dir(root, "afx/src/afx");
    make_dir(root, "afx/notes");
    make_dir(root, "broken");
    make_dir(root, "broken/src");
    make_dir(root, "broken/src/brk");
    write_file(root, "afx/build.tur",
               "(defpackage afx :name \"afx\"\n"
               "  :spices #map{\"afdep\" #map{:path \"../afdep\"}})\n");
    make_dir(root, "afdep");
    make_dir(root, "afdep/src");
    make_dir(root, "afdep/src/afdep");
    write_file(root, "afdep/build.tur", "(defpackage afdep :name \"afdep\")\n");
    write_file(root, "afdep/src/afdep/util.tur",
               "(defmodule afdep/util (export twice)\n"
               "  (defn twice [x : int] : int\n"
               "    ```c\n"
               "    int64_t r = 0;\n"
               "    for (int i = 0; i < 2; i++) r += x;\n"
               "    return r;\n"
               "    ```))\n");
    write_file(root, "afx/src/afx/core.tur", FIXTURE);
    write_file(root, "broken/build.tur", "(defpackage brk :name \"brk\")\n");
    write_file(root, "broken/src/brk/core.tur",
               "(defmodule brk/core (export f) (defn f [] : int (undefined-thing)))\n");

    char note[1024];
    snprintf(note, sizeof(note), "%s/afx/notes/walkthrough.tur.md", root);
    const char *imp =
        "(import afx/core :refer [add-loop wrapped box-new box-get box-free Pt pt-sum answer\n"
    "                         pt-make B3 b3-sum b3-make apply-sum count-if\n"
    "                         Pf pf-apply pf-sample crv-new crv-at crv-free\n"
    "                         papply-sum pcount3 ppf-apply ppf-sample pb3 fb3])";

    /* Control: without the image, the first inline-C call is the refusal. */
    TuriEnv *ctl = turi_env_new();
    turi_env_set_toplevel_imports(ctl, true);
    turi_env_set_search_path_for(ctl, note);
    TuriValue vc = turi_eval(ctl, imp);
    CHECK(!turi_is_error(vc), "control: the import resolves without an image");
    vc = turi_eval(ctl, "(add-loop 40 2)");
    CHECK(turi_is_error(vc), "control: an inline-C loop is refused by the interpreter");
    turi_env_free(ctl);

    TuriEnv *env = turi_env_new();
    turi_env_set_toplevel_imports(env, true);
    turi_env_set_search_path_for(env, note);
    int n = turi_env_attach_spice_for_module(env, "afx/core", tur);
    /* add-loop wrapped box-new box-get box-free answer greet even-word?
     * dep-twice pt-sum pt-make b3-sum b3-make apply-sum count-if pf-apply
     * pf-sample crv-new crv-at crv-free papply-sum pcount3 ppf-apply
     * ppf-sample pb3 fb3 -- not tagged-id (no definition), not the
     * dep's `twice` (not this spice's export). */
    CHECK(n == 26, "attach: binds the twenty-six exports the FFI can marshal");
    CHECK(turi_env_attach_spice_for_module(env, "afx/core", tur) == 0,
          "attach: a module an attached image provides is not loaded twice");
    CHECK(turi_env_attach_spice_for_module(env, "no/such-module", tur) == 0,
          "attach: a module outside any spice is not an error");

    TuriValue v = turi_eval(env, imp);
    CHECK(!turi_is_error(v), "attached: the import still evaluates");
    CHECK(is_int(turi_eval(env, "(add-loop 40 2)"), 42),
          "attached: an inline-C export runs compiled");
    CHECK(is_int(turi_eval(env, "(wrapped 5)"), 16),
          "attached: importing the module keeps a wrapper's compiled export");
    CHECK(is_int(turi_eval(env,
              "(let [b (box-new 7)] (let [s (box-get b)] (box-free b) s))"), 14),
          "attached: an opaque-handle API round-trips through the image");
    CHECK(is_int(turi_eval(env, "(pt-sum (Pt 3 4))"), 13),
          "attached: a small record argument crosses by value");
    CHECK(is_int(turi_eval(env, "(let [p (pt-make 2 5)] (+ (* 100 (. p x)) (. p y)))"), 605),
          "attached: a small record result comes back as a record");
    CHECK(is_int(turi_eval(env, "(b3-sum (B3 3 20 100))"), 129),
          "attached: a large record argument crosses as a pointer");
    CHECK(is_int(turi_eval(env, "(b3-sum (b3-make 1 20 100))"), 129),
          "attached: a large record result feeds the next call");
    CHECK(is_int(turi_eval(env, "(afx/core/answer)"), 42),
          "attached: the qualified name reaches the export");
    CHECK(is_int(turi_eval(env, "(afx/core/dep-twice 20)"), 41),
          "attached: an export calling into a :spices dep runs compiled");
    TuriValue g = turi_eval(env, "(afx/core/greet)");
    CHECK(g.tag == TURI_CSTR && g.as_cstr && strcmp(g.as_cstr, "hello") == 0,
          "attached: a :cstr export returns the string");
    TuriValue ev = turi_eval(env, "(afx/core/even-word? 6)");
    CHECK(ev.tag == TURI_BOOL && ev.as_bool, "attached: a :bool export returns a boolean");
    /* Callbacks.  A float probe with a fractional part (0.5 + i, times
     * 1.5): 0.75 + 2.25 + 3.75. */
    turi_eval(env, "(defn times-1-5 [x : float] : float (* x 1.5))");
    CHECK(is_float(turi_eval(env, "(apply-sum times-1-5 3)"), 6.75),
          "callback: a cell's defn is called back from compiled code");
    CHECK(is_float(turi_eval(env, "(apply-sum times-1-5 3)"), 6.75),
          "callback: a second call reuses the same box");
    CHECK(is_float(turi_eval(env,
              "(let [k 2.5] (apply-sum (fn [x : float] : float (* x k)) 3))"), 11.25),
          "callback: a capturing fn literal crosses too");
    turi_eval(env, "(defn odd-int? [i : int] : bool (= (mod i 2) 1))");
    CHECK(is_int(turi_eval(env, "(count-if odd-int? 7)"), 3),
          "callback: a :bool result reaches the compiled caller");
    turi_eval(env, "(defn pf-sum [p : Pf] : float (+ (. p x) (. p y)))");
    CHECK(is_float(turi_eval(env, "(pf-apply pf-sum 1.25)"), 7.5),
          "callback: a record argument is rebuilt for the cell");
    turi_eval(env, "(defn pf-at [t : float] : Pf (Pf t (* t 3.0)))");
    CHECK(is_float(turi_eval(env, "(pf-sample pf-at 0.75)"), 3.0),
          "callback: a record result is packed for the compiled caller");
    CHECK(is_float(turi_eval(env, "(let [c (crv-new times-1-5)] (let [y (crv-at c 7.1)] (crv-free c) y))"), 7.1 * 1.5),
          "callback: compiled code keeps the handle and calls it on a later call");
    CHECK(turi_is_error(turi_eval(env, "(apply-sum 3 3)")),
          "callback: a non-function in a callback slot is refused");
    /* Plain fn parameters: the same values through the poly carrier. */
    CHECK(is_float(turi_eval(env, "(papply-sum times-1-5)"), 6.75),
          "plain callback: a cell's defn is called through {env, fn}");
    CHECK(is_float(turi_eval(env,
              "(let [k 2.5] (papply-sum (fn [x : float] : float (* x k))))"), 11.25),
          "plain callback: a capturing fn literal crosses too");
    CHECK(is_int(turi_eval(env, "(pcount3 odd-int?)"), 2),
          "plain callback: a :bool result reaches the compiled caller");
    CHECK(is_float(turi_eval(env, "(ppf-apply pf-sum 1.25)"), 3.75),
          "plain callback: a record argument is rebuilt for the cell");
    CHECK(is_float(turi_eval(env, "(ppf-sample pf-at 0.75)"), 4.5),
          "plain callback: a record result is packed for the compiled caller");
    turi_eval(env, "(defn b3-total [r : B3] : int (+ (. r a) (+ (. r b) (. r c))))");
    CHECK(is_int(turi_eval(env, "(pb3 b3-total)"), 321),
          "plain callback: a wide record argument arrives through its box");
    CHECK(is_int(turi_eval(env, "(fb3 b3-total)"), 654),
          "callback: a wide record argument arrives through its box");

    turi_eval(env, "(defn wrapped [x : int] : int 0)");
    CHECK(is_int(turi_eval(env, "(wrapped 5)"), 0),
          "attached: a cell's own defn of the same name takes over");
    turi_env_free(env);

    /* A bare name the host already binds keeps the host's binding. */
    TuriEnv *sh = turi_env_new();
    turi_env_register_native(sh, "answer", native_seven, NULL);
    turi_env_set_search_path_for(sh, note);
    CHECK(turi_env_attach_spice(sh, root, tur) < 0,
          "attach: a directory with no build.tur above it is an error");
    char spice[1100];
    snprintf(spice, sizeof(spice), "%s/afx", root);
    CHECK(turi_env_attach_spice(sh, spice, tur) == 26, "attach by root: same twenty-six bindings");
    CHECK(is_int(turi_eval(sh, "(answer)"), 7),
          "attach: a host-bound bare name is not clobbered");
    CHECK(is_int(turi_eval(sh, "(afx/core/answer)"), 42),
          "attach: the shadowed export answers to its qualified name");
    turi_env_free(sh);

    /* A spice that does not build fails once per env, not once per import:
     * once the source is fixed, the same env still answers from its record
     * (no rebuild was attempted) while a fresh env builds and binds it. */
    TuriEnv *bk = turi_env_new();
    snprintf(spice, sizeof(spice), "%s/broken", root);
    CHECK(turi_env_attach_spice(bk, spice, tur) < 0, "broken spice: the attach fails");
    write_file(root, "broken/src/brk/core.tur",
               "(defmodule brk/core (export f) (defn f [] : int 3))\n");
    CHECK(turi_env_attach_spice(bk, spice, tur) < 0,
          "broken spice: a second attach in the same env does not rebuild");
    turi_env_free(bk);
    TuriEnv *fx = turi_env_new();
    CHECK(turi_env_attach_spice(fx, spice, tur) == 1, "fixed spice: a fresh env attaches it");
    turi_env_free(fx);

    char rm[1100];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", root);
    if (system(rm) != 0) fprintf(stderr, "embed-attach-spice: could not remove %s\n", root);

    if (failures == 0) {
        printf("\nAll embed-attach-spice tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
