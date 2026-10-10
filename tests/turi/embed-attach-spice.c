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

static TuriValue native_seven(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_int(7);
}

static const char *FIXTURE =
    "(defmodule afx/core\n"
    "  (import afdep/util :refer [twice])\n"
    "  (export add-loop wrapped box-new box-get box-free tagged-id Pt pt-sum answer\n"
    "          greet even-word? dep-twice B3 pt-make b3-sum b3-make)\n"
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
    "                         pt-make B3 b3-sum b3-make])";

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
     * dep-twice pt-sum pt-make b3-sum b3-make -- not tagged-id (no
     * definition), not the dep's `twice` (not this spice's export). */
    CHECK(n == 13, "attach: binds the thirteen exports the FFI can marshal");
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
    CHECK(turi_env_attach_spice(sh, spice, tur) == 13, "attach by root: same thirteen bindings");
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
