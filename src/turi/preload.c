/* preload.c -- shared stdlib preload sequence for interpreter entry points.
 *
 * See preload.h for the rationale.  Relocated from the inline blocks in
 * src/main.c's cmd_eval so the native `--interpret` path and the WASM REPL
 * (src/web/wasm_glue.c) share one load list.
 */

#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE   /* setenv */
#endif
#include "turi/preload.h"

#include "turi/eval.h"
#include "buf.h"
#include "source_literal.h"
#include "runtime/globals.h"   /* saffron-lang-plan S6: g_lang_prelude */
#include "turi/collections_native.h"   /* turi_register_collection_natives */
#include "turi/interpreter_natives.h" /* turi_env_register_interpreter_natives */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Resolve NULL/"" to the legacy cwd-relative default. */
static const char *preload_root(const char *stdlib_root) {
    return (stdlib_root && stdlib_root[0]) ? stdlib_root : "stdlib";
}

/* interp-stdlib-class-method-shadows-user-defn: preload turns run with
 * stdlib_prefix == 0, so the in_stdlib_load bracket never covers them; this
 * flag is what marks the typeclasses they register as stdlib-owned (see
 * runtime/globals.h).  Set for the duration of each preload helper. */
extern bool g_turi_stdlib_preload;

/* Emit and evaluate `(load "<root>/<base>")`.  `root` is a filesystem path
 * going into generated SOURCE, so it needs escaping -- see source_literal.h. */
static void preload_one(TuriEnv *env, const char *root, const char *base) {
    char form[4300], eroot[8200];
    if (!tur_source_literal_escape(root, eroot, sizeof eroot)) return;
    snprintf(form, sizeof form, "(load \"%s/%s\")", eroot, base);
    TuriValue sv = turi_eval(env, form);
    (void)sv; /* a failed stdlib load surfaces via the env's diag sink */
}

void turi_env_preload_macros(TuriEnv *env, const char *stdlib_root) {
    if (!env) return;
    bool saved_preload = g_turi_stdlib_preload;
    g_turi_stdlib_preload = true;
    const char *root = preload_root(stdlib_root);
    /* Each in its own eval so the file_id / Phase M7 promotion ordering that
     * cmd_eval documents holds: macros first (and/or/when/cond/for/...), then
     * contract (assert!/require!/ensure!/invariant!). */
    preload_one(env, root, "macros.tur");
    preload_one(env, root, "contract.tur");
    g_turi_stdlib_preload = saved_preload;
}

void turi_env_preload_native_stubs(TuriEnv *env) {
    if (!env) return;
    /* Inject typed stubs so the elaborator knows the signatures of the native
     * functions used by benchmark scripts AND the untyped carrier-list ops
     * (nil-value/cons/head/tail).  Without these, a bare `cons`/`head`/`tail`
     * resolves to the elaborator's BS_FUNC_CALL builtin, which the tree-walker's
     * eval_builtin cannot execute -- its default arm silently returns nil (see
     * src/turi/eval.c).  That is exactly the REPL-only
     * `(list-head (cons 65 (cons 66 0))) => nil` divergence
     * (docs/archive/repl-list-head-over-cons-returns-nil.md): under
     * `--interpret` these stubs make `cons` a user-defn call the runtime native
     * (registered by wk_register_stdlib_natives, which overrides the stub body)
     * actually services, so the same expression returns 65.  Loaded AFTER
     * turi_env_preload_macros and BEFORE turi_env_preload_collections so the real
     * module defns own any overlapping name, and BEFORE
     * turi_env_register_interpreter_natives so the native shims, registered last,
     * override any inline-C module body the interpreter cannot execute.  The
     * stubs whose names ARE defined by a preloaded collection module (vec-get,
     * ok?/some?/err?, ...) are deliberately omitted to avoid the "already defined
     * by an auto-loaded stdlib module" collision. */
    TuriValue sv = turi_eval(env,
        /* list operations */
        "(defn nil-value [] :int 0)\n"
        /* Head is a polymorphic tyvar, not :int, so the stub matches the
         * compiled-path `cons` builtin's wildcard head (elab_call.c's
         * cons_wildcard bypass): a cons cell is a pointer-as-int64 carrier and
         * accepts any 64-bit-sized head -- int, cstr, opaque handle.  Typing it
         * :int made the elaborator reject a cstr head (`(cons "a" 0)`) under
         * --interpret while the compiled path accepted it, the re-string parity
         * gap.  The tail stays :int (the carrier) and the return stays :int; the
         * runtime native (native_cons) boxes the head through intptr_t exactly
         * as codegen does. */
        "(defn cons [A] [v :A n :int] :int 0)\n"
        "(defn head [lst :int] :int 0)\n"
        "(defn tail [lst :int] :int 0)\n"
        /* vec operations.  vec-get/vec-set!/vec-free are dropped here -- the real
         * vec.tur (preloaded next) defines them, and a stub would collide with
         * "already defined by an auto-loaded stdlib module".  vec-new-filled is
         * benchmark-only (no module defn) and its stub is declared below, in
         * turi_env_preload_collections, AFTER vec.tur loads -- see the comment
         * there for why it cannot be declared here. */
        /* numeric helpers.  cstr->parse-int / bit-shr / bit-xor stubs are
         * dropped: cstr->parse-int is a documented interpreter-only native
         * (c-integration-guide.md) that runtime-dispatches by design;
         * bit-shr/bit-xor are real compiler builtins (src/compiler/
         * builtins.c) the elaborator already knows the signature of. A
         * :int stub for either would shadow that builtin behavior.
         *
         * int->float is NOT a compiler builtin (no builtins.c entry,
         * despite this comment previously claiming otherwise) -- it is a
         * plain native (native_int_to_float, interpreter_natives.c) with no
         * stub at all, so the elaborator has never seen its signature: a
         * bare call warns TUR-W0040 "unknown name" and the result types as
         * unconstrained, which then fails as "mixed-width numeric
         * arithmetic" the moment it feeds a float context (e.g.
         * monte_carlo_pi.tur's turi variant). Needs the same real stub
         * int->unit-float/tur-sqrt already get below. */
        "(defn println-float [x :float d :int] :nil nil)\n"
        "(defn int->float [x :int] : float 0.0)\n"
        "(defn int->unit-float [x :int] :float 0.0)\n"
        "(defn tur-sqrt [x :float] :float 0.0)\n"
        /* HAMT operations for hash_map benchmark */
        "(defn hamt-new [] :int 0)\n"
        "(defn hamt-free [m :int] :nil nil)\n"
        "(defn hamt-set [m :int hash :int key :int val :int] :int 0)\n"
        "(defn hamt-get [m :int hash :int key :int] :int 0)\n"
        "(defn hamt-hash-ptr [p :int] :int 0)\n"
        /* I/O benchmark helpers (file_read.tur, file_write.tur) */
        "(defn write-temp-file [path :cstr n :int] :nil nil)\n"
        "(defn io-fopen-read [path :cstr] :int 0)\n"
        "(defn io-fread-chunk [fp :int buf :int] :int 0)\n"
        "(defn io-fclose [fp :int] :nil nil)\n"
        "(defn io-remove [path :cstr] :nil nil)\n"
        "(defn io-buf-new [] :int 0)\n"
        "(defn io-buf-free [buf :int] :nil nil)\n"
        "(defn io-alloc [n :int v :int] :int 0)\n"
        "(defn io-free [buf :int] :nil nil)\n"
        "(defn io-fopen-write [path :cstr] :int 0)\n"
        "(defn io-fwrite-chunk [fp :int buf :int offset :int chunk :int] :int 0)\n"
        /* Whole-benchmark natives (random_access, thread_ring, nbody, ray_tracing) */
        "(defn random-access-bench [size :int reads :int] :int 0)\n"
        "(defn run-ring [n :int m :int] :nil nil)\n"
        "(defn run-nbody [n :int steps :int] :nil nil)\n"
        "(defn run-raytracer [w :int h :int] :int 0)\n"
        /* none? predicate signature the elaborator needs so the native types as
         * :bool (not :int, which trips the strict "if condition must be bool"
         * check).  ok?/err?/some? are dropped because result.tur / option.tur
         * (both preloaded next) define them; none? has no module defn. */
        "(defn none? [r :int] :bool false)\n"
        /* PS3: what the `doc` macro expands to.  A native (native_doc_print);
         * the stub is what lets the elaborator type the call instead of
         * warning TUR-W0040 at every `(doc ...)`.  doc-lookup gets no stub: it
         * would collide with an explicit (load "stdlib/docstrings.tur"), whose
         * body the native overrides anyway. */
        "(defn doc-print [name :cstr] :nil nil)\n"
    );
    (void)sv;
}

void turi_env_preload_collections(TuriEnv *env, const char *stdlib_root) {
    if (!env) return;
    bool saved_preload = g_turi_stdlib_preload;
    g_turi_stdlib_preload = true;
    const char *root = preload_root(stdlib_root);

    /* The typeclass-stub + typed-collection set the compiled path auto-loads.
     * Kept in sync with cmd_eval's `prelude[]` in src/main.c.  contract.tur is
     * NOT here (it is loaded up front by turi_env_preload_macros, next to
     * macros.tur, because the M7 macro promotion is order-sensitive).  sym.tur
     * is appended last so first-class :Sym ops (and the Hash/Eq[Sym] instances
     * a keyword-keyed map needs) are available. */
    static const char *prelude[] = {
        "safe.tur",
        "typeclass-eq.tur", "typeclass-ord.tur", "typeclass-functor.tur",
        "typeclass-clone.tur",
        "typeclass-drop.tur",
        "typeclass-hash.tur", "typeclass-applicative.tur",
        "typeclass-alternative.tur", "typeclass-monad.tur",
        "typeclass-monaderror.tur", "typeclass-bifunctor.tur",
        "hamt.tur", "set.tur", "map.tur",
        "vec.tur", "slice.tur", "option.tur", "result.tur",
        "pair.tur", "tuple.tur", "list.tur", "grid.tur", "zipper.tur",
        "mutmap.tur",
        "unique.tur",
        "sym.tur",
        /* SX1: backtrackable state.  Every binding is inline-C the tree-walker
         * cannot run, so this is only usable because wk_register_trail_natives
         * overrides the bodies -- which is why that registration must run AFTER
         * this preload, not before. */
        "trail.tur",
        /* RM3 A1: `with-region`, the lifetime-only region bracket.  Unlike
         * trail.tur it needs NO native: the body is a bare Turmeric forwarder
         * `(body)`, which the tree-walker runs as an identity call -- which is
         * also exactly what the compiled path does with `--enable=regions` off,
         * the only mode the interpreter has (a region is a codegen decision at
         * the emitter's call site, and the interpreter has no emitter).  Listed
         * here for module-preload parity with the compiled autoload
         * (tools/check_turi_native_parity.py), not for any behaviour. */
        "region.tur",
        NULL
    };

    /* Build one `(load A)(load B)...` blob so the whole prelude elaborates in a
     * single turi_eval -- distinct file_ids, deps resolved in declaration
     * order. */
    Buf src;
    buf_init(&src);
    char eroot[8200];
    if (!tur_source_literal_escape(root, eroot, sizeof eroot)) return;
    for (int i = 0; prelude[i] != NULL; i++) {
        buf_puts(&src, "(load \"");
        buf_puts(&src, eroot);
        buf_putc(&src, '/');
        buf_puts(&src, prelude[i]);
        buf_puts(&src, "\")\n");
    }
    /* saffron-lang-plan S6 / r7rs-lang-plan R1: the ENTRY file's language
     * prelude (LangTraits.prelude, e.g. `saffron/prelude.tur`), when it has
     * one.  LAST, so every stdlib name it adapts is already bound.
     *
     * The compiled path appends the same file to `tur_stdlib_autoload_files()`;
     * this list is its interpreter twin and the two are kept in sync by hand
     * (tools/check_turi_native_parity.py checks module-preload parity), so the
     * append has to happen in both or `tur --interpret` reports
     * "unknown name 'vec-map'" on a program the compiler accepts. */
    if (g_lang_prelude) {
        buf_puts(&src, "(load \"");
        buf_puts(&src, eroot);
        buf_putc(&src, '/');
        buf_puts(&src, g_lang_prelude);
        buf_puts(&src, "\")\n");
    }
    buf_putc(&src, '\0'); /* turi_eval calls strlen; NUL-terminate */
    TuriValue sv = turi_eval(env, src.data);
    (void)sv;
    buf_free(&src);

    /* vec-new-filled: benchmark-only native (native_vec_new_filled in
     * collections_native.c), no module defn of its own.  Its stub cannot live
     * in turi_env_preload_native_stubs above -- that runs BEFORE vec.tur (just
     * loaded here) defines the Vec struct, so a `(Vec A)` return-type
     * annotation there hits "cannot apply a type of kind '*' as a type
     * constructor" (Vec unbound).  native_vec_new_filled builds the exact
     * {data,len,cap} layout defstruct Vec expects, so once Vec exists the
     * stub can and must return (Vec A) -- typing it :int (as it did
     * previously) type-checks vec-new-filled's own call site but then fails
     * every downstream vec-get/vec-set!/vec-free call on the same value,
     * since those are real vec.tur functions requiring (Vec A), not a raw
     * int handle.
     *
     * NOTE: this fixes the static type only. At runtime the interpreter
     * still executes this stub's own body (an empty vec-new) instead of
     * native_vec_new_filled, because turi_register_collection_natives (which
     * owns this native) is registered exactly once, at env-creation time
     * (turi/env.c), with no later re-assertion after preload -- unlike
     * turi_env_register_interpreter_natives, which is deliberately called a
     * second time in main.c AFTER all turi_env_preload_* calls specifically
     * so a native shim wins over any stub/module body declared in between.
     * A real fix re-registers (at minimum) vec-new-filled -- or all of
     * turi_register_collection_natives -- at that same late point. See
     * docs/reported/turi-vec-new-filled-native-override-lost.md. */
    TuriValue sv2 = turi_eval(env,
        "(defn vec-new-filled [A] [n :int v :A] : (Vec A) (:: (vec-new) (Vec A)))\n"
    );
    (void)sv2;
    g_turi_stdlib_preload = saved_preload;
}

void turi_env_preload_typeclasses(TuriEnv *env, const char *stdlib_root) {
    if (!env) return;
    bool saved_preload = g_turi_stdlib_preload;
    g_turi_stdlib_preload = true;
    /* Load ONLY typeclass-show.tur (Show class + primitive instances +
     * Show[Vec]/Show[Set]/Show[Map]), not the full typeclass.tur.  The rest of
     * typeclass.tur (Error/Display/Debug[ptr<void>]) has inline-C instance
     * methods -- notably Error's error-message -- that would shadow interpreter
     * builtins the async runtime relies on.  Loaded after
     * turi_env_preload_collections so the collection Show instances see their
     * backing Vec/Set/Map types. */
    preload_one(env, preload_root(stdlib_root), "typeclass-show.tur");
    g_turi_stdlib_preload = saved_preload;
}

void turi_env_preload_stdlib(TuriEnv *env, const char *stdlib_root) {
    if (!env) return;
    /* $TUR_STDLIB_DIR first, as for `tur` itself and the R7RS embed. */
    const char *root = getenv("TUR_STDLIB_DIR");
    if (!root || !*root) root = stdlib_root;
    if (root && *root && !getenv("TUR_STDLIB_DIR")) {
        /* A module import's stdlib fallback and the stdlib's own
         * `(load "stdlib/...")` forms resolve through TUR_STDLIB_DIR
         * (elab_toplevel.c, reader.c), which `tur` exports for itself; an
         * embedder that names its root gets the same (r7rs_embed.c does too). */
#ifdef _WIN32
        _putenv_s("TUR_STDLIB_DIR", root);
#else
        setenv("TUR_STDLIB_DIR", root, 1);
#endif
    }
    /* The program preload `tur --interpret` runs (src/main.c cmd_eval), in
     * its order; see each helper's comment in preload.h for why it sits where
     * it does.  Not the REPL's Show slice (turi_env_preload_typeclasses): it
     * loads stdlib `String` into the global scope, where a module that
     * defines its own (frame/ownstr) can no longer export it. */
    turi_env_preload_macros(env, root);
    turi_env_preload_native_stubs(env);
    turi_env_preload_collections(env, root);
    /* A `#lang` switch truncates back to here, not to an empty session. */
    turi_env_pin_prelude(env);
    /* The native shims win over the inline-C bodies just loaded; the
     * collection registry is re-asserted for the same reason (vec-new-filled). */
    turi_env_register_interpreter_natives(env);
    turi_register_collection_natives(env);
}
