/* cmd_eval.c -- the interpreter-backed subcommands: `tur interpret` and `tur
 * debug` (cmd_eval_inc), `tur eval`, `tur repl`, `tur dap` and `tur trace`.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

#include "source_literal.h"
#include "reader_macros.h"
#include "lang_dialects.h"  /* `tur dialects`: the `#lang` base axis */
#include "turi/eval.h"
#include "turi/collections_native.h"
#include "turi/interpreter_natives.h"
#include "turi/preload.h"
#include "turi/repl.h"
#include "globals.h"
#include "turi/dap.h"
#include "turi/trace.h"

static bool file_defines_main(const char *path);

/* The wk_register_* / native_contract_* natives now live in
 * src/turi/interpreter_natives.c (tur_core); their declarations come in via
 * "turi/interpreter_natives.h" above. */

/* Debugger Phase 3: optional hook fired by cmd_eval_h (in debug mode) once the
 * interpreter env + debugger are constructed but before the program is armed
 * and run.  The DAP launch path uses it to flush staged breakpoints and install
 * the DAP pause / condition handlers.  NULL for the plain `tur debug` REPL. */
typedef struct {
    void (*on_ready)(TuriEnv *env, void *ud);
    /* Fired once, immediately before the env is freed. Anything the embedder
     * installed ON the env -- a pause handler, above all -- has to come off
     * while the env is still there to take it off. */
    void (*on_done)(TuriEnv *env, void *ud);
    void  *ud;
} EvalHooks;

/* Phase S0: tur repl — interactive read-eval-print loop. */
/* Phase INT-1: run a .tur file through the tree-walking interpreter.
 * extra_argv/extra_argc are the arguments after the file path, exposed
 * to the script as *args* (a cons-cell list of C-string pointers). */
static int cmd_eval_h(const char *path, bool use_color,
                      char **extra_argv, int extra_argc, bool debug,
                      const EvalHooks *hooks,
                      char **user_inc, int n_user_inc) {
    g_interpret_mode = true;
    /* The enclosing project's `:experiments` apply to a file run through the
     * interpreter as they do to one that is checked or compiled.  Before the
     * env is built: experiments are read while it elaborates. */
    apply_enclosing_manifest_experiments(path);
    turi_init(use_color);
    TuriEnv *env = turi_env_new();
    if (!env) {
        fprintf(stderr, "tur: failed to create interpreter environment\n");
        return 1;
    }
    /* File-eval (`tur --interpret <file>`) is a batch compile of a single
     * file, not an interactive session: a reader macro registered twice with
     * a differing template is the same "two modules both register #json"
     * footgun the compiled entry points reject (TURI_FIXTURES errors/
     * reader-macros-strict-collision).  Make this env's reader-macro registry
     * strict so the collision is a hard error, matching the compiled path.
     * The REPL (turi/repl.c) leaves the default strict=false so its src_acc
     * replay and iterative redefinition stay smooth.  Safe here because the
     * preloaded stdlib registers no reader macros and the user file is parsed
     * in a single pass (no self-replay of its own `reader-macros/define`). */
    env->reader_macros->strict = true;
    /* A debug session used to opt OUT of incremental elaboration here, because
     * the DAP server resolves each frame's source from diag_file_path(
     * span.file_id) and the incremental path lost that provenance -- a
     * post-stepOut frame came back as `?:19` with no `source` object, and the
     * conditional breakpoint never fired.  The cause was the file REGISTRY, not
     * the spans: diag_reset() clears it every turn, and the incremental path
     * reuses previously-parsed Forms rather than re-running their `(load ...)`
     * splices, so the files those Forms name were never re-registered.  Fixed
     * at the source (diag_files_save / diag_files_restore in turi_eval_impl),
     * so debug sessions keep the incremental path.
     * See docs/archive/incremental-elab-loses-span-file-provenance.md. */
    /* Pre-detect the user file's #lang so the prelude loads under the SAME
     * reader.  Otherwise the user file's `#lang sweet-exp` (etc.) flips
     * env->reader_type mid-stream, and turi_eval_impl discards the accumulated
     * prelude (src_acc) to avoid re-parsing it under the new reader -- dropping
     * every preloaded stdlib defn, so e.g. a `#map{...}` literal under
     * `#lang sweet-exp` fails "unknown function or operator 'hamt-of'".  By
     * setting the reader first, the prelude is parsed under the file's reader
     * from the start (plain s-expr parses under every reader variant) and the
     * user file's directive then matches env->reader_type -- no reset fires.
     * Scoped to the file-eval entry point; the REPL keeps its protective reset
     * for genuine interactive reader switches. */
    {
        FILE *pf = fopen(path, "rb");
        if (pf) {
            char head[512];
            size_t hn = fread(head, 1, sizeof(head) - 1, pf);
            fclose(pf);
            head[hn] = '\0';
            const char *rest = head; size_t rest_len = hn;
            LangDialect dialect = LANG_TURMERIC;
            ReaderType rt = detect_lang_dialect(head, hn, &rest, &rest_len,
                                                NULL, NULL, &dialect);
            /* A `.scm` file is Scheme with no directive to find: the
             * extension seeds the same three things a `#lang r7rs` line
             * would.  Without this the Scheme prelude was not the language
             * prelude, so an on-demand `(import (scheme read))` spliced a
             * second copy of it and failed to elaborate. */
            LangDialect ext_dialect = lang_dialect_from_extension(path);
            if (ext_dialect != LANG_TURMERIC && rest == head) {
                rt = reader_type_from_extension(path);
                dialect = ext_dialect;
            }
            if ((rest != head || ext_dialect != LANG_TURMERIC) && reader_type_is_implemented(rt)) {
                env->reader_type = rt;
                /* saffron-lang-plan S1: seed the LANGUAGE axis here for the
                 * same reason the reader is seeded -- the prelude must load
                 * under the same language as the user file, and this runs
                 * before the eval blob that would otherwise be the first place
                 * the directive is seen. */
                env->lang = dialect;
                /* saffron-lang-plan S6: and the Saffron PRELUDE flag, for the
                 * same reason one line up.  `turi_env_preload_collections` runs
                 * below and reads it to decide whether to append
                 * `saffron/prelude.tur`, so seeding it only in
                 * `detect_and_adjust_lang` (which the compiled paths use) left
                 * `tur --interpret` reporting "unknown name 'vec-map'" on a
                 * program the compiler accepted. */
                g_lang_prelude = lang_traits(dialect)->prelude;
            }
        }
    }
    /* Preload macros.tur so that and/or/when/cond/for etc. are available.
     * This is the minimum stdlib needed for any real Turmeric program to work.
     *
     * TI8.b (turi-parity-post-v1-plan): load it via a `(load ...)` form rather
     * than turi_eval_file().  turi_eval_file concatenates the source into the
     * single <eval> blob (file_id 0); macros.tur carries `(defmodule tur/macros
     * ...)`, so any user fixture that *also* declares a defmodule then collided
     * with it under "only one defmodule is allowed per file" (both forms shared
     * file_id 0, defeating the per-file reset).  The `(load ...)` path assigns
     * macros.tur its own file_id, so the defmodule-per-file boundary reset
     * fires and a user defmodule no longer conflicts with the preloaded one.
     *
     * Runtime contracts (contract.tur) are loaded right after macros.tur (and,
     * like it, in its own turi_eval so it lands at the very front of the
     * accumulated source).  contract.tur exports macros (assert!/require!/
     * ensure!/invariant! + their -msg! variants); the Phase M7 promotion in
     * elaborate_program nulls a tur/-prefixed module's macros'
     * defining_module_name -- making them globally visible without an explicit
     * import -- only for modules within the stdlib-prefix boundary, which
     * (because load-expansion shifts form indices) effectively covers the
     * earliest-loaded modules.  Loading contract.tur up front, next to
     * macros.tur, keeps assert!/require!/... in that promoted region so user
     * code sees them bare under --interpret.  Its inline-C tur-contract-check /
     * tur-contract-check-inv bodies are overridden by native_contract_check[_inv]
     * below; without this preload the elaborator never sees a tur-contract-check
     * binding and silently drops every :pre/:post/:type check (a silent
     * miscompile).
     *
     * turi_env_preload_macros (src/turi/preload.c) is the shared helper the WASM
     * REPL also calls, so the two entry points cannot drift. */
    turi_env_preload_macros(env, resolve_stdlib_root());
    /* Inject typed stubs so the elaborator knows the signatures of the native
     * functions used by benchmark scripts and the carrier-list ops.  The native
     * shims registered below replace these no-op closures at runtime.  Factored
     * into turi_env_preload_native_stubs (src/turi/preload.c) so the REPL shares
     * the exact same set and cannot drift (the missing stubs there were the
     * `(list-head (cons ...)) => nil` REPL bug). */
    turi_env_preload_native_stubs(env);
    /* TI8.b/W1 (turi-interpreter-gap-closure-plan): preload the typeclass-stub
     * + typed-collection stdlib set that the compiled path auto-loads in
     * compile_to_c().  Without it the interpreter cannot resolve Cons/Option/
     * Result struct types, the Eq/Clone/Hash/HKT typeclasses, or the
     * map-new/vec-eq?/tcons/... collection functions -- so every fixture that
     * touches the typed stdlib errored under --interpret while passing when
     * compiled.
     *
     * Loaded via (load ...) (not turi_eval_file): several of these modules carry
     * their own (defmodule ...), and only the (load ...) preprocessing assigns
     * each file a distinct file_id so the per-file one-defmodule reset fires
     * (same root cause as the macros.tur fix above).  Loaded AFTER the benchmark
     * stub block (with the 6 overlapping stubs dropped) so the real module defns
     * own those names, and BEFORE wk_register_stdlib_natives so the native shims,
     * registered last, override any inline-C module body the interpreter cannot
     * execute.  Reader-backed modules (json/schema/sym) follow the same -X gates
     * as the compiled path.
     *
     * The array (safe, typeclass-*, hamt/set/map, vec/slice, option/result,
     * pair/tuple/list, grid/zipper, mutmap, unique, sym) and its batched-load
     * shape now live in turi_env_preload_collections (src/turi/preload.c), the
     * shared helper the WASM REPL also calls so the two entry points cannot
     * drift.  contract.tur is NOT in that list -- it is loaded up front next to
     * macros.tur (Phase M7 macro promotion is order-sensitive; see that load
     * site).  The inline-C bodies of these modules (map/set ops, contract
     * checks, mutmap, ...) are overridden by the native_* shims registered below
     * (wk_register_stdlib_natives et al.), which is why this preload runs BEFORE
     * that registration.  See docs/archive/history/turi-map-set-hamt-interpreter-gap.md
     * and docs/archive/history/web-repl-missing-stdlib-preload.md. */
    turi_env_preload_collections(env, resolve_stdlib_root());
    /* JR0/RD (turi-json-schema-interpreter-plan, Layers 1-2): auto-load
     * json.tur, then schema.tur on top of it, so the #json(...) reader-macro
     * lowering's json node constructors (json/encode|decode|type|get) and the
     * #json-str<T>(...) typed-decode reader family resolve under --interpret.
     * The loaded inline-C bodies are overridden by the layout-exact natives
     * registered in wk_register_{json,schema}_natives below.  Loaded BEFORE the
     * native overrides so those still win.  These two modules are therefore
     * unconditionally preloaded under --interpret (they are gaps only relative
     * to the static prelude[] above; docs/artifacts/turi-preload-carve-out.txt). */
    {
        extern bool g_turi_stdlib_preload;   /* stdlib-owned class marking */
        g_turi_stdlib_preload = true;
        char pb[4096];
        tur_stdlib_path("json.tur", pb, sizeof(pb));
        char load_form[4200], epb[8200];
        if (!tur_source_literal_escape(pb, epb, sizeof epb)) epb[0] = '\0';
        snprintf(load_form, sizeof(load_form), "(load \"%s\")", epb);
        TuriValue sv = turi_eval(env, load_form);
        (void)sv;
    }
    {
        char pb[4096];
        tur_stdlib_path("schema.tur", pb, sizeof(pb));
        char load_form[4200], epb[8200];
        if (!tur_source_literal_escape(pb, epb, sizeof epb)) epb[0] = '\0';
        snprintf(load_form, sizeof(load_form), "(load \"%s\")", epb);
        TuriValue sv = turi_eval(env, load_form);
        (void)sv;
    }
    {
        extern bool g_turi_stdlib_preload;
        g_turi_stdlib_preload = false;   /* preload done: user turns follow */
    }
    /* Pin the accumulated preload (prelude + json/schema above) so an inline
     * `#lang` directive in the evaluated program truncates src_acc back to here
     * rather than emptying it (web-repl-lang-switch-drops-stdlib). */
    turi_env_pin_prelude(env);
    /* Register the full interpreter native override set (stdlib inline-C
     * shims, contracts, safe/typeclass/comonad/mutex/future/bytes/taskgroup/
     * chan/backtrack/proc-fs/serial/sym/seq/json/schema).  Relocated into
     * src/turi/interpreter_natives.c so the WASM REPL and `tur repl` register
     * the same block and cannot drift.  Must run AFTER turi_env_preload_*. */
    turi_env_register_interpreter_natives(env);
    /* Re-assert the collection natives (vec/set/map/hamt) for the same
     * reason: unlike turi_env_register_interpreter_natives, this registry is
     * otherwise only populated once, at env-creation time (turi/env.c),
     * before any preload runs. Every name it owns that also gets a
     * Turmeric-level defn during preload -- currently only vec-new-filled,
     * whose stub lives in turi_env_preload_collections -- would otherwise
     * keep running that stub body instead of the native. See
     * docs/archive/turi-vec-new-filled-native-override-lost.md. */
    turi_register_collection_natives(env);
    /* Build *args* as a cons-cell list of C-string pointers.  The cells are
     * this function's: freed after the env below (the strings are argv's). */
    typedef struct { int64_t value; int64_t next; } TurCons;
    int64_t args_list = 0;
    {
        for (int i = extra_argc - 1; i >= 0; i--) {
            TurCons *c = (TurCons *)malloc(sizeof(TurCons));
            c->value = (int64_t)(intptr_t)extra_argv[i];
            c->next  = args_list;
            args_list = (int64_t)(intptr_t)c;
        }
        TuriValue args_val = {0};
        args_val.tag    = TURI_INT;
        args_val.as_int = args_list;
        turi_env_set(env, "*args*", args_val);
        /* *argv0*: the script path, the nearest thing to a compiled binary's
         * argv[0] (r7rs-command-line-first-element-is-tur). */
        turi_env_set(env, "*argv0*", turi_cstr(path));
    }
    /* Set module_base_dir so (import ...) resolves relative to the script.
     *
     * Through the SETTER, not a direct field assignment: the field carries an
     * ownership flag (`module_base_dir_owned`) that only the setter raises, and
     * `turi_env_free` frees the string only when it is raised.  A direct
     * assignment is the BORROWED path -- correct for a pointer into `argv`, and
     * a leak for a fresh allocation, which is what this used to hand it.
     * The setter strdups, so the slice can live on the stack.
     * See docs/archive/cli-mallocs-module-base-dir-through-the-borrowed-path.md */
    {
        const char *slash = strrchr(path, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - path);
            char *dpath = (char *)malloc(dlen + 1);
            if (dpath) {
                memcpy(dpath, path, dlen);
                dpath[dlen] = '\0';
                turi_env_set_module_base_dir(env, dpath);
                free(dpath);
            }
        }
    }
    /* Module search path: the caller's -I dirs first (the elaborator takes the
     * first match), then the enclosing spice's src/ and every :spices dep's
     * src/ -- the list `tur check` / `tur run <file>` build, so a program that
     * compiles from inside a spice also interprets there.  --no-auto-spice
     * skips the walk-up.  A dir already on the list (the `tur run
     * --engine=interp` caller has appended the walk-up itself) is dropped.
     * The env borrows the array; it is freed after the env below.
     * See docs/archive/interpret-takes-no-include-path-or-spice-discovery.md */
    char **eval_inc = NULL;
    int    n_eval_inc = 0;
    char **eval_inc_owned = NULL;
    int    n_eval_inc_owned = 0;
    if (n_user_inc > 0) {
        eval_inc = (char **)malloc((size_t)n_user_inc * sizeof(char *));
        if (eval_inc) {
            memcpy(eval_inc, user_inc, (size_t)n_user_inc * sizeof(char *));
            n_eval_inc = n_user_inc;
        }
    }
    auto_append_spice_includes(path, &eval_inc, &n_eval_inc,
                               &eval_inc_owned, &n_eval_inc_owned, NULL);
    {
        int w = 0;
        for (int r = 0; r < n_eval_inc; r++) {
            bool dup = false;
            for (int k = 0; k < w && !dup; k++)
                dup = strcmp(eval_inc[k], eval_inc[r]) == 0;
            if (!dup) eval_inc[w++] = eval_inc[r];
        }
        n_eval_inc = w;
    }
    env->include_dirs   = (const char **)eval_inc;
    env->n_include_dirs = n_eval_inc;
    /* Debugger Phase 2: attach the debugger before top-level eval so the
     * (break) builtin resolves while the file is read; it stays UNARMED so
     * prelude + top-level forms run without stopping.  We arm it right before
     * main() below. */
    /* The (break) builtin is always available under the interpreter: a no-op
     * with no debugger attached, a pause trigger under `tur debug`.  Register
     * it before top-level eval so `(break)` resolves either way. */
    turi_debug_register_break_builtin(env);
    if (debug)
        turi_debug_enable(env, stdin, stdout);
    /* Phase 3 (DAP): now that the env + debugger exist, let the embedder stage
     * breakpoints and install its pause handler before any program node runs. */
    if (debug && hooks && hooks->on_ready)
        hooks->on_ready(env, hooks->ud);
    /* The user file is brought in via `(load "path")` rather than
     * turi_eval_file's concatenate-into-<eval> path: a loaded file gets its own
     * file_id and keeps its real path + 1-based line numbers, so diagnostics,
     * breakpoints (`break <line>`), source listings, and stack frames resolve
     * against the user's source instead of the synthetic <eval> blob (which
     * offsets every line by the preloaded prelude).
     *
     * incremental-elab-loses-span-file-provenance: this used to be the debug
     * path only, and plain `--interpret` went through turi_eval_file -- so a
     * type error in the user's file was reported as `<eval>:64:10` instead of
     * `file.tur:3:10`.  That half of the report was never actually about
     * incremental elaboration (TUR_NO_INCREMENTAL_ELAB=1 reproduced it
     * identically); it is the eval-blob concatenation, which both paths shared.
     * Routing both through `(load ...)` fixes it at the source.
     *
     * This became possible only once the load splicer learned to honour a
     * loaded file's inline `#lang` directive (load-ignores-inline-lang-directive,
     * elab_toplevel.c) -- before that, switching the entry file onto this route
     * broke every `#lang`/sweet-exp fixture that did not also carry a
     * dialect-bearing extension. */
    TuriValue result;
    /* debugger-and-tracer-only-instrument-main: a file with no `main` is a
     * top-level program, so arm around the load itself -- the entry stop
     * lands on its first located node, breakpoints in it and in what it calls
     * fire, and the recorder records its steps (it used to report 0 steps and
     * `tur dap` never stopped, not even for stopOnEntry).  A file WITH a main
     * keeps arming right before `(main)` below, so its top-level definitions
     * still load without stopping. */
    bool has_main = file_defines_main(path);
    if (debug && !has_main) turi_debug_arm(env);
    {
        char load_form[4200];
        char epath[8200];
    if (!tur_source_literal_escape(path, epath, sizeof epath)) epath[0] = '\0';
    snprintf(load_form, sizeof load_form, "(load \"%s\")", epath);
        result = turi_eval(env, load_form);
    }
    int rc = 0;
    if (turi_is_error(result)) {
        const char *msg = turi_error_message(result);
        /* parse/elaboration errors are already printed by the diagnostic system */
        if (msg && strcmp(msg, "parse error") != 0 &&
                   strcmp(msg, "elaboration error") != 0) {
            fprintf(stderr, "tur: %s\n", msg);
        }
        rc = 1;
    } else {
        TuriValue main_fn = turi_env_get(env, "main");
        if (main_fn.tag == TURI_CLOSURE) {
            if (debug) turi_debug_arm(env);
            TuriValue r = turi_call(env, main_fn, NULL, 0);
            /* A runtime error (or uncaught throw) raised while running main was
             * previously swallowed -- main exited non-zero with no message.
             * Print it to stderr, mirroring the top-level error path above, so
             * `tur --interpret` surfaces the diagnostic (e.g. an unknown call
             * head) instead of failing silently. */
            if (r.tag == TURI_ERROR) {
                const char *msg = turi_error_message(r);
                if (msg && strcmp(msg, "parse error") != 0 &&
                           strcmp(msg, "elaboration error") != 0) {
                    fprintf(stderr, "tur: %s\n", msg);
                }
                rc = 1;
            }
            else if (r.tag == TURI_THROW) {
                /* TuriThrow is opaque here; print a generic notice (the inner
                 * message lives in eval.c) and fail non-zero. */
                fprintf(stderr, "tur: uncaught exception\n");
                rc = 1;
            }
            else if (r.tag == TURI_INT) rc = (int)r.as_int;
        }
        turi_run_pending_defers(env);
    }
    if (hooks && hooks->on_done) hooks->on_done(env, hooks->ud);
    turi_env_free(env);
    while (args_list) {
        TurCons *c = (TurCons *)(intptr_t)args_list;
        args_list = c->next;
        free(c);
    }
    free(eval_inc);
    for (int i = 0; i < n_eval_inc_owned; i++) free(eval_inc_owned[i]);
    free(eval_inc_owned);
    return rc;
}

/* debugger-and-tracer-only-instrument-main: does the file define a top-level
 * `main`?  A textual scan -- `(defn main` / `defn main` at a line start (the
 * sweet-exp spelling) -- because the answer is needed BEFORE the file is
 * loaded: it decides whether the debugger is armed around the whole load (a
 * top-level program: its forms ARE the program, so the entry stop, the
 * breakpoints and the recorder must see them) or, as before, only around the
 * `(main)` call.  The same rule Try Turmeric and Trowel re-derive on the
 * client side; deriving it here lets them stop. */
static bool file_defines_main(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[4096];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "(defn main", 10) == 0 || strncmp(p, "defn main", 9) == 0) {
            char c = p[(p[0] == '(') ? 10 : 9];
            if (c == ' ' || c == '\t' || c == '[' || c == '(' || c == '\n' || c == '\r')
                found = true;
        }
    }
    fclose(f);
    return found;
}

/* The common case with no embedder hooks; `user_inc` is the caller's -I dirs
 * (borrowed for the call, may be NULL). */
int cmd_eval_inc(const char *path, bool use_color,
                 char **user_inc, int n_user_inc,
                 char **extra_argv, int extra_argc, bool debug) {
    return cmd_eval_h(path, use_color, extra_argv, extra_argc, debug, NULL,
                      user_inc, n_user_inc);
}

/* Debugger Phase 3: DAP launch glue.  The DAP server calls dap_launch_cb once
 * the client has finished configuration; it runs the program under the
 * interpreter debugger (cmd_eval_h, debug mode) and wires the DAP pause /
 * conditional handlers in via the on_ready hook before the program is armed. */
static void dap_on_ready_cb(TuriEnv *env, void *ud) {
    dap_begin_session(ud, env);   /* ud is the opaque DapState* */
}

/* The mirror of dap_on_ready_cb: a replay session's recorder is installed on
 * the env, so it has to be taken off before the env is freed. */
static void dap_on_done_cb(TuriEnv *env, void *ud) {
    dap_end_session(ud, env);
}

static int dap_launch_cb(const char *program, char **args, int n_args,
                         void *state, void *ud) {
    (void)ud;
    EvalHooks hooks = { dap_on_ready_cb, dap_on_done_cb, state };
    /* use_color=false: stdout is the (captured) debuggee channel; diagnostics go
     * to stderr without colour, matching `tur lsp` / `tur mcp`. */
    return cmd_eval_h(program, /*use_color=*/false, args, n_args,
                      /*debug=*/true, &hooks, NULL, 0);
}

int cmd_dap(void) {
    diag_init(false);   /* no color -- stdout is reserved for JSON-RPC */
    return dap_server_run(STDIN_FILENO, STDOUT_FILENO, dap_launch_cb, NULL);
}

/* ---------------------------------------------------------------------------
 * `tur trace` -- the time-travel recorder (track T1)
 *
 * Runs a program under the interpreter with the tracer installed, then either
 * writes the recording or prints what it caught. The smallest thing that
 * proves the recorder, and a post-mortem tool in its own right even before
 * anything replays the bytes.
 * ------------------------------------------------------------------------ */

typedef struct {
    TurTrace     *trace;
    TurTraceOpts  opts;
} TraceCtx;

static void trace_on_ready_cb(TuriEnv *env, void *ud) {
    TraceCtx *ctx = (TraceCtx *)ud;
    ctx->trace = turi_trace_begin(env, &ctx->opts);
}

/* The recorder's pause handler lives on the env, so it has to be taken off
 * before the env is. The recording itself survives -- it is our bytes, not
 * the interpreter's. */
static void trace_on_done_cb(TuriEnv *env, void *ud) {
    (void)env;
    TraceCtx *ctx = (TraceCtx *)ud;
    turi_trace_stop(ctx->trace);
}

int cmd_trace(const char *path, const char *out_path,
              char **extra_argv, int extra_argc, uint32_t max_steps,
              TurTraceGrain grain) {
    TraceCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.opts.max_steps      = max_steps;
    ctx.opts.capture_output = true;
    ctx.opts.grain          = grain;

    EvalHooks hooks = { trace_on_ready_cb, trace_on_done_cb, &ctx };
    /* debug=true is what installs the debugger and arms it; the tracer's
     * pause handler then sees every node. */
    int rc = cmd_eval_h(path, /*use_color=*/false, extra_argv, extra_argc,
                        /*debug=*/true, &hooks, NULL, 0);

    if (!ctx.trace) {
        fprintf(stderr, "tur: trace: could not attach the recorder\n");
        return rc ? rc : 1;
    }
    turi_trace_stop(ctx.trace);

    TurTraceStats st;
    memset(&st, 0, sizeof st);
    turi_trace_stats(ctx.trace, &st);

    size_t len = 0;
    const uint8_t *bytes = turi_trace_bytes(ctx.trace, &len);

    int wrote = 0;
    if (out_path && bytes) {
        FILE *f = fopen(out_path, "wb");
        if (!f) {
            fprintf(stderr, "tur: trace: cannot write %s\n", out_path);
            turi_trace_end(ctx.trace);
            return 1;
        }
        wrote = fwrite(bytes, 1, len, f) == len;
        fclose(f);
        if (!wrote) {
            fprintf(stderr, "tur: trace: short write to %s\n", out_path);
            turi_trace_end(ctx.trace);
            return 1;
        }
    }

    fprintf(stderr,
            "trace: %u steps (%s), %u enters, %u pops, %u changes, "
            "peak depth %u, %zu bytes, %u bytes of output, truncated %s\n",
            st.steps, grain == TUR_TRACE_GRAIN_LINE ? "per line" : "per expression",
            st.enters, st.pops, st.changes, st.peak_depth,
            len, st.output_bytes, st.truncated ? "yes" : "no");
    if (out_path && wrote)
        fprintf(stderr, "trace: wrote %s\n", out_path);

    turi_trace_end(ctx.trace);
    return rc;
}

/* Read a .turtrace back and print one line per record.
 *
 * The format is exercised by a reader before any UI depends on it, which is
 * the ordering the plan asks for: if the format is wrong, a timeline widget is
 * the expensive place to find that out. */
int cmd_trace_dump(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "tur: trace: cannot read %s\n", path);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return 1; }
    rewind(f);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return 1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    TurTraceReader r;
    if (!turi_trace_open(&r, buf, got)) {
        fprintf(stderr, "tur: trace: %s is not a readable .turtrace\n", path);
        free(buf);
        return 1;
    }
    printf("turtrace v%u  names=%u sites=%u records=%zu bytes  "
           "steps=%s  truncated=%s\n",
           r.version, r.name_count, r.site_count, r.record_bytes,
           r.node_grain ? "per expression" : "per line",
           r.truncated ? "yes" : "no");

    TurTraceRecord rec;
    while (turi_trace_next(&r, &rec)) {
        TurTraceSite site;
        uint16_t fl = 0, nl = 0;
        const char *file = "", *fn = "";
        char span[32];
        span[0] = '\0';
        if (turi_trace_site(&r, rec.site, &site)) {
            file = turi_trace_name(&r, site.file_name, &fl);
            fn   = turi_trace_name(&r, site.fn_name, &nl);
            /* `line:col-end` when the recording carries a range, `line:col`
             * when it is a v1 file that only ever had a point. */
            if (site.col_end > site.col)
                snprintf(span, sizeof span, "%u:%u-%u",
                         site.line, site.col, site.col_end);
            else
                snprintf(span, sizeof span, "%u:%u", site.line, site.col);
        }
        switch (rec.tag) {
        case TUR_TRACE_ENTER:
            printf("ENTER  depth=%u %.*s:%s %.*s\n", rec.depth,
                   (int)fl, file, span, (int)nl, fn);
            break;
        case TUR_TRACE_POP:
            printf("POP    depth=%u\n", rec.depth);
            break;
        case TUR_TRACE_STEP:
            printf("STEP   depth=%u %.*s:%s %.*s", rec.depth,
                   (int)fl, file, span, (int)nl, fn);
            for (uint16_t i = 0; i < rec.n_changes; i++) {
                uint32_t nid = 0;
                const char *repr = "";
                uint16_t rl = 0, bnl = 0;
                if (!turi_trace_change(&rec, i, &nid, &repr, &rl)) break;
                const char *bn = turi_trace_name(&r, nid, &bnl);
                printf("  %.*s=%.*s", (int)bnl, bn, (int)rl, repr);
            }
            printf("\n");
            break;
        case TUR_TRACE_OUTPUT:
            printf("OUTPUT %u bytes\n", rec.payload_len);
            break;
        default:
            break;
        }
    }
    free(buf);
    return 0;
}

/* E3: tur eval '<expr>' — evaluate an inline expression and print result. */
int cmd_eval_expr(const char *expr, bool use_color) {
    g_interpret_mode = true;
    turi_init(use_color);
    TuriEnv *env = turi_env_new();
    if (!env) {
        fprintf(stderr, "tur: failed to create interpreter environment\n");
        return 1;
    }
    TuriValue result = turi_eval(env, expr);
    int rc = 0;
    if (turi_is_error(result)) {
        const char *msg = turi_error_message(result);
        if (msg && strcmp(msg, "parse error") != 0 &&
                   strcmp(msg, "elaboration error") != 0) {
            fprintf(stderr, "tur: %s\n", msg);
        }
        rc = 1;
    } else if (result.tag != TURI_NIL) {
        char repr[512];
        turi_value_repr(repr, sizeof(repr), result);
        printf("%s\n", repr);
    }
    turi_env_free(env);
    return rc;
}

int cmd_repl(bool watch_mode) {
    /* UCH0 (diagnose-unbound-call-heads-plan): the REPL is an interpreter
     * entry point -- flag it so the INT-1 reader conditional picks :turi and
     * so elab_call keeps the runtime-native dispatch fallback (UCH1). */
    g_interpret_mode = true;

    /* Resolve (and export via TUR_STDLIB_DIR) the stdlib root so turi_repl_run's
     * shared preload can (load ...) the prelude even when the REPL is launched
     * from outside the repo tree. */
    resolve_stdlib_root();

    /* UC-3 (user-config-experiments-plan): honor the user-level experiments
     * file for the interactive REPL, unless the enclosing project's build.tur
     * declares its own :experiments (which suppresses it).  CLI --enable=
     * (already applied in the global flag pass) still wins.  We read the
     * manifest only to decide suppression; the REPL's manifest-experiment
     * behavior is otherwise unchanged. */
    {
        char cwd[4096];
        char *root = getcwd(cwd, sizeof(cwd)) ? drv_find_project_root(cwd) : NULL;
        char mp[4096];
        PkgManifest m;
        memset(&m, 0, sizeof(m));
        if (root && pkg_resolve_manifest_path(root, mp, sizeof(mp))
            && pkg_manifest_read(mp, &m)) {
            apply_user_config_experiments(&m);
        } else {
            apply_user_config_experiments(NULL);
        }
        pkg_manifest_free(&m);
        free(root);
    }

    return turi_repl_run(watch_mode);
}
