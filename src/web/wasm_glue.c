/* wasm_glue.c - WASM-friendly API wrapper for libturi
 *
 * This file provides a C API that is designed to be easily callable from
 * WebAssembly via Emscripten. It wraps the existing libturi eval API with
 * functions that:
 *   - Have simple, flat signatures (no struct pointers in args)
 *   - Allocate and free memory in a WASM-compatible way
 *   - Return results as strings for easy JS interop
 */

#include "wasm_glue.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* We need to include the turi headers. src/ is in the include path. */
#include "turi/compiled_turn.h"
#include "turi/env.h"
#include "turi/eval.h"
#include "turi/value.h"
#include "arena.h"
#include "buf.h"
#include "diag.h"
#include "fmt.h"
#include "lang_dialects.h"
#include "reader.h"
#include "runtime/experiments.h"
#include "runtime/globals.h"   /* r7rs-lang-plan R9: g_lang_prelude */
#include "source_literal.h"  /* tur_source_literal_escape */
#include "symbols.h"
#include "turi/preload.h"
#include "turi/docstrings.h"
#include "turi/interpreter_natives.h"
#include "turi/repl.h"
#include "elab.h"
#include "expr.h"
#include "forms.h"
#include "types.h"

/* ---------------------------------------------------------------------------
 * Global state for the WASM module
 * ---------------------------------------------------------------------------
 */

/* The single evaluation environment for the WASM module.
 * This is initialized once and reused across all eval calls. */
static TuriEnv *g_env = NULL;

static void wasm_diag_sink(struct TuriEnv *env, int level, const char *code,
                           const char *file, uint32_t line,
                           uint32_t col_start, uint32_t col_end,
                           const char *message, void *ud);

static void type_of_diag_sink(struct TuriEnv *env, int level, const char *code,
                              const char *file, uint32_t line,
                              uint32_t col_start, uint32_t col_end,
                              const char *message, void *ud);

/* ---------------------------------------------------------------------------
 * Memory management helpers
 * ---------------------------------------------------------------------------
 */

/* Allocate memory that can be freed by JavaScript via _free().
 * Emscripten's EM_MALLOC_POOL is used by malloc in WASM builds. */
char *turi_wasm_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

/* Free memory allocated by turi_wasm_strdup. */
void turi_wasm_free(void *p) {
    free(p);
}

/* Root of the stdlib inside the WASM MEMFS.  The Emscripten link step bundles
 * the host `stdlib/` tree here via `--embed-file <src>/stdlib@/stdlib`, and the
 * module's default working directory is "/", so a cwd-relative `stdlib/<file>`
 * load (the form the stdlib modules use internally for their transitive loads)
 * and this top-level root resolve to the same `/stdlib/<file>` -- keeping the
 * (load ...) dedup keys consistent. */
#define WASM_STDLIB_ROOT "stdlib"

/* performance-audit-plan WP5: the wall time of each startup phase, so Try
 * Turmeric can report what a first visit waits for (W-2).  Only the first
 * turi_wasm_init records -- a reset re-runs the preload, but it is not a cold
 * start.  Read back as JSON with turi_wasm_boot_timings(). */
#define WASM_BOOT_MAX_PHASES 12
static struct { const char *name; double ms; } g_boot[WASM_BOOT_MAX_PHASES];
static int    g_boot_n;
static double g_boot_prev;
static bool   g_boot_recording;

static double wasm_now_ms(void) {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    /* Native builds of this file are unit tests; they never read the times. */
    return (double)clock() * 1000.0 / CLOCKS_PER_SEC;
#endif
}

static void wasm_boot_mark(const char *phase) {
    if (!g_boot_recording) return;
    double now = wasm_now_ms();
    if (g_boot_n < WASM_BOOT_MAX_PHASES) {
        g_boot[g_boot_n].name = phase;
        g_boot[g_boot_n].ms   = now - g_boot_prev;
        g_boot_n++;
    }
    g_boot_prev = now;
}

/* Bring a freshly-created env up to interpreter parity with the native
 * `tur --interpret` entry point: load the core macros (when/cond/for/and/or +
 * assert!/require!/...) and the typed-collection stdlib so reader-macro data
 * literals (`#map{...}`, `#set{...}`, `[...]`) and the rest of the stdlib
 * resolve instead of failing "unknown function or operator 'hamt-of'".  The
 * underlying Vec/Set/Map/HAMT runtime ops are already registered as natives by
 * turi_env_new (turi_register_collection_natives).  Shares the load list with
 * `tur --interpret` via src/turi/preload.c so the two entry points cannot drift.
 * See docs/archive/history/web-repl-missing-stdlib-preload.md. */
static void wasm_preload_stdlib(TuriEnv *env) {
    if (!env) return;
    turi_env_preload_macros(env, WASM_STDLIB_ROOT);
    wasm_boot_mark("preload_macros");
    /* Typed native-function stubs (nil-value/cons/head/tail + benchmark helpers)
     * in the same after-macros/before-collections slot as --interpret and the
     * native REPL, so `(list-head (cons 65 (cons 66 0)))` evaluates to 65 at the
     * browser prompt instead of nil (cons would otherwise be an elaborator
     * builtin the tree-walker cannot execute). */
    turi_env_preload_native_stubs(env);
    wasm_boot_mark("preload_native_stubs");
    turi_env_preload_collections(env, WASM_STDLIB_ROOT);
    wasm_boot_mark("preload_collections");
    /* Preload the REPL-only Show slice (Show [Vec] / [Set] / [Map]) so a
     * collection result renders through its Show instance via
     * turi_try_show_by_tag below, matching the native `tur repl`
     * (src/turi/repl.c, right after turi_env_preload_collections). */
    turi_env_preload_typeclasses(env, WASM_STDLIB_ROOT);
    wasm_boot_mark("preload_typeclasses");
    /* Pin the preloaded prefix: the browser REPL is the long-lived env this
     * matters most for -- running an editor program that opens with
     * `#lang turmeric/sweet` used to wipe src_acc, so the next `#map{}` at the
     * prompt failed as "unknown function or operator 'hamt-of'"
     * (web-repl-lang-switch-drops-stdlib). */
    turi_env_pin_prelude(env);
    /* Register the interpreter native overrides (sym/:Sym, contracts, seq,
     * json/schema, safe box/unbox, comonad/mutex/future/chan/...) so the WASM
     * REPL can evaluate ops whose stdlib body is inline-C -- e.g. `#map{:a 1}`
     * needs the Hash[Sym]/Eq[Sym] natives, `assert!` needs the contract natives.
     * Relocated from src/main.c into tur_core so this entry point registers the
     * same block as --interpret and cannot drift (web-repl-repl-inline-c-native-
     * gap).  Runs AFTER the preload so the native shims win over the loaded
     * inline-C bodies. */
    turi_env_register_interpreter_natives(env);
    wasm_boot_mark("pin_and_natives");
    /* PS5: snapshot the runtime prelude last, so it holds the native overrides
     * registered just above -- turi_wasm_rewind_to_prelude restores it. */
    turi_env_snapshot_prelude(env);
    wasm_boot_mark("snapshot_prelude");
}

/* Push a program's partial last line out of libc's buffer.
 *
 * Emscripten's stdout is a character device, so libc line-buffers it: text
 * with no trailing newline stays in the FILE buffer, never reaching the JS
 * side at all.  Scheme's `display` writes no newline, so
 * `(display "Hello, world!")` printed NOTHING in Try Turmeric -- and then
 * surfaced glued to the front of the next run that happened to emit one.
 *
 * Every entry point that can run user code calls this on its way out.  The
 * page's other half of the fix is in web/public/eval-worker.js, which takes
 * the bytes per-byte (Module.stdout) instead of letting Emscripten's TTY layer
 * hold back a line of its own.  Both are needed: this one gets the text out of
 * libc, that one gets it onto the page. */
static void wasm_flush_program_output(void) {
    fflush(stdout);
    fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * Public API functions (exported to WASM)
 * ---------------------------------------------------------------------------
 */

/* Initialize the Turmeric runtime for WASM.
 * Must be called once before any other turi_wasm_* function.
 * Returns 0 on success, non-zero on failure. */
int turi_wasm_init(void) {
    if (g_env) return 0; /* Already initialized */

    g_boot_recording = true;
    g_boot_prev = wasm_now_ms();

    /* Use the unrestricted environment — the browser's WASM sandbox already
     * prevents real filesystem/network access.  Turmeric's sandboxed flag
     * would block println/print which we want routed to the JS print callback. */
    g_env = turi_env_new();
    if (!g_env) return 1;
    turi_env_set_diag_sink(g_env, wasm_diag_sink, g_env);
    wasm_boot_mark("env_new");

    /* Initialize diagnostics (no color in WASM - output goes to JS console) */
    turi_init(false);
    wasm_boot_mark("turi_init");

    /* Preload the stdlib (macros + typed collections) so the REPL matches the
     * native --interpret path -- without this, `#map{}`/`#set{}` and every
     * macro (when/cond/for/...) failed as "unknown function or operator". */
    wasm_preload_stdlib(g_env);

    g_boot_recording = false;
    return 0;
}

/* The phases turi_wasm_init recorded, as a JSON object of milliseconds in the
 * order they ran ({"env_new":1.2,...}); "{}" before init.  The string is
 * static; the caller must NOT free it. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_wasm_boot_timings(void) {
    static char buf[64 * WASM_BOOT_MAX_PHASES + 4];
    size_t n = 0;
    buf[n++] = '{';
    for (int i = 0; i < g_boot_n && n < sizeof buf - 2; i++) {
        int w = snprintf(buf + n, sizeof buf - n - 1, "%s\"%s\":%.3f",
                         i ? "," : "", g_boot[i].name, g_boot[i].ms);
        if (w < 0 || (size_t)w >= sizeof buf - n - 1) break;
        n += (size_t)w;
    }
    buf[n++] = '}';
    buf[n] = '\0';
    return buf;
}

/* Reset the evaluation environment to a fresh state.
 * This clears all definitions and returns to a clean slate. */
void turi_wasm_reset(void) {
    if (g_env) {
        /* r7rs-lang-plan R9: a reset keeps the session's language (its
         * prelude is seeded before the preload). */
        LangDialect keep_lang = g_env->lang;
        ReaderType  keep_rt   = g_env->reader_type;
        turi_env_free(g_env);
        g_env = turi_env_new();
        if (g_env) {
            turi_env_set_diag_sink(g_env, wasm_diag_sink, g_env);
            g_env->lang        = keep_lang;
            g_env->reader_type = keep_rt;
            g_lang_prelude     = lang_traits(keep_lang)->prelude;
            /* Re-establish stdlib parity on the fresh env (matches init). */
            wasm_preload_stdlib(g_env);
        }
    }
}

void turi_wasm_rewind_to_prelude(void) {
    if (g_env) turi_env_rewind_to_prelude(g_env);
}

/* Evaluate a Turmeric source string and return the result as a string.
 * 
 * Arguments:
 *   input - The Turmeric source code to evaluate (NUL-terminated)
 * 
 * Returns:
 *   A malloc'd string containing the result representation, or NULL on error.
 *   The caller is responsible for freeing this with turi_wasm_free().
 * 
 * On parse/runtime error, the error message is returned as the result string.
 */
/* aot-compiled-repl-plan C2 at the page's prompt: turn compiled turns on or
 * off for the session (`--enable=compiled-repl`, per session).  Only a module
 * with an engine has a compiled-turn hook (wasm_jit_glue.c); elsewhere this
 * stays off.  Returns whether they are on. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int turi_wasm_set_compiled_repl(int on) {
    if (!g_env) return 0;
    g_env->compiled_repl = on && turi_compiled_turn_available();
    return g_env->compiled_repl ? 1 : 0;
}

char *turi_wasm_eval(const char *input) {
    if (!g_env) return turi_wasm_strdup("Error: Turmeric runtime not initialized. Call turi_wasm_init() first.");
    if (!input) return turi_wasm_strdup("Error: NULL input");

    turi_repl_set_last_diag_code("");

    char type_tag[64] = {0};
    TuriValue result = turi_eval_typed(g_env, input, type_tag, sizeof(type_tag));
    wasm_flush_program_output();

    /* aot-compiled-repl-plan C2: a compiled turn rendered its own result. */
    if (g_env->last_turn_printed && g_env->last_turn_text)
        return turi_wasm_strdup(g_env->last_turn_text);

    /* SI4: four-tier display:
     *   1. turi_try_show        -- TURI_STRUCT with Show instance
     *   2. turi_show_result     -- TURI_INT heap-pointer (Pair, Cons)
     *   3. turi_try_show_by_tag -- TURI_INT named ADT/struct/coll
     *                              (Vec, Set, Map, ...) via its Show
     *   4. turi_value_repr      -- default repr (fallback below) */
    const char *show_str = turi_try_show(g_env, result);
    if (!show_str)
        show_str = turi_show_result(g_env, result, type_tag);
    if (!show_str)
        show_str = turi_try_show_by_tag(g_env, result, type_tag);
    if (show_str) {
        char *ret = turi_wasm_strdup(show_str);
        free((char *)show_str);
        return ret;
    }

    char buf[2048];
    turi_value_repr(buf, sizeof(buf), result);
    return turi_wasm_strdup(buf);
}

/* Evaluate a Turmeric source string and return both result and any error.
 * 
 * This is useful when you want to distinguish between a successful evaluation
 * that returns a value, and an error case.
 * 
 * Arguments:
 *   input - The Turmeric source code to evaluate (NUL-terminated)
 *   out_result - Output pointer for the result string (malloc'd, NULL on error)
 *   out_error - Output pointer for the error string (malloc'd, NULL on success)
 * 
 * Returns:
 *   0 on success (result in *out_result, *out_error is NULL)
 *   1 on error (error message in *out_error, *out_result is NULL)
 * 
 * Caller must free both *out_result and *out_error with turi_wasm_free().
 */
int turi_wasm_eval_ex(const char *input, char **out_result, char **out_error) {
    if (!g_env) {
        *out_result = NULL;
        *out_error = turi_wasm_strdup("Error: Turmeric runtime not initialized. Call turi_wasm_init() first.");
        return 1;
    }
    if (!input) {
        *out_result = NULL;
        *out_error = turi_wasm_strdup("Error: NULL input");
        return 1;
    }
    
    *out_result = NULL;
    *out_error = NULL;

    turi_repl_set_last_diag_code("");

    char type_tag[64] = {0};
    TuriValue result = turi_eval_typed(g_env, input, type_tag, sizeof(type_tag));
    wasm_flush_program_output();

    if (turi_is_error(result)) {
        char buf[2048];
        turi_value_repr(buf, sizeof(buf), result);
        *out_error = turi_wasm_strdup(buf);
        return 1;
    }
    if (g_env->last_turn_printed && g_env->last_turn_text) {
        *out_result = turi_wasm_strdup(g_env->last_turn_text);
        return 0;
    }

    /* SI4: four-tier display:
     *   1. turi_try_show        -- TURI_STRUCT with Show instance
     *   2. turi_show_result     -- TURI_INT heap-pointer (Pair, Cons)
     *   3. turi_try_show_by_tag -- TURI_INT named ADT/struct/coll
     *                              (Vec, Set, Map, ...) via its Show
     *   4. turi_value_repr      -- default repr (fallback below) */
    const char *show_str = turi_try_show(g_env, result);
    if (!show_str)
        show_str = turi_show_result(g_env, result, type_tag);
    if (!show_str)
        show_str = turi_try_show_by_tag(g_env, result, type_tag);
    if (show_str) {
        *out_result = turi_wasm_strdup(show_str);
        free((char *)show_str);
        return 0;
    }

    char buf[2048];
    turi_value_repr(buf, sizeof(buf), result);
    *out_result = turi_wasm_strdup(buf);
    return 0;
}

static int wasm_run_program_here(const char *path);

/* Run the program in the file at `path` the way `tur --interpret` does
 * (cmd_interpret_main, src/driver/cmd_eval.c), on a fresh environment:
 * `(load "path")`, then `main` if the file defines one, then the pending
 * defers.  Returns the exit status: main's int result, or 1 when loading or
 * main reported an error (printed to stderr).  The browser JIT's fallback
 * (web/wasm_jit_glue.c): a program, unlike a REPL line, runs its main.
 *
 * The fresh environment is the program's own.  The session's -- in the
 * browser JIT, the Compile engine's prompt (wasm-jit-plan P6) -- is set aside
 * and put back afterwards, so a Run that fell back does not take the prompt's
 * definitions with it. */
int turi_wasm_run_program(const char *path) {
    if (turi_wasm_init() != 0) return 1;
    TuriEnv *session = g_env;
    const char *session_prelude = g_lang_prelude;
    g_env = NULL;
    int rc = 1;
    if (turi_wasm_init() == 0) {
        rc = wasm_run_program_here(path);
        turi_env_free(g_env);
    }
    g_env = session;
    g_lang_prelude = session_prelude;
    return rc;
}

static int wasm_run_program_here(const char *path) {
    if (!g_env || !path) return 1;
    /* The program's #lang picks the session's language first, as the page
     * does before an Interpret run: a language with its own prelude (r7rs) or
     * stdlib (saffron) needs the session built for it, and `load` alone only
     * switches the reader.  No #lang is turmeric -- a reset keeps the previous
     * program's language, which must not leak into this one. */
    {
        char lang[128] = "turmeric";
        FILE *f = fopen(path, "rb");
        char head[160];
        if (f) {
            size_t n = fread(head, 1, sizeof head - 1, f);
            fclose(f);
            head[n] = '\0';
            if (strncmp(head, "#lang ", 6) == 0) {
                size_t len = strcspn(head + 6, "\r\n");
                while (len > 0 && (head[6 + len - 1] == ' ' || head[6 + len - 1] == '\t')) len--;
                if (len > 0 && len < sizeof lang) {
                    memcpy(lang, head + 6, len);
                    lang[len] = '\0';
                }
            }
        }
        if (turi_wasm_set_lang(lang) != 0) {
            fprintf(stderr, "tur: unknown #lang '%s'\n", lang);
            wasm_flush_program_output();
            return 1;
        }
    }
    char epath[8200];
    if (!tur_source_literal_escape(path, epath, sizeof epath)) return 1;
    char load_form[8300];
    snprintf(load_form, sizeof load_form, "(load \"%s\")", epath);
    int rc = 0;
    TuriValue result = turi_eval(g_env, load_form);
    if (turi_is_error(result)) {
        const char *msg = turi_error_message(result);
        /* parse and elaboration errors were printed by the diagnostic system */
        if (msg && strcmp(msg, "parse error") != 0 && strcmp(msg, "elaboration error") != 0)
            fprintf(stderr, "tur: %s\n", msg);
        rc = 1;
    } else {
        TuriValue main_fn = turi_env_get(g_env, "main");
        if (main_fn.tag == TURI_CLOSURE) {
            TuriValue r = turi_call(g_env, main_fn, NULL, 0);
            if (r.tag == TURI_ERROR) {
                const char *msg = turi_error_message(r);
                if (msg && strcmp(msg, "parse error") != 0 && strcmp(msg, "elaboration error") != 0)
                    fprintf(stderr, "tur: %s\n", msg);
                rc = 1;
            } else if (r.tag == TURI_THROW) {
                fprintf(stderr, "tur: uncaught exception\n");
                rc = 1;
            } else if (r.tag == TURI_INT) {
                rc = (int)r.as_int;
            }
        }
        turi_run_pending_defers(g_env);
    }
    wasm_flush_program_output();
    return rc;
}

/* Free a string allocated by turi_wasm_eval or turi_wasm_eval_ex. */
void turi_wasm_free_string(char *s) {
    free(s);
}

/* Get the version of the Turmeric runtime.
 * Returns a static string - do not free. */
const char *turi_wasm_version(void) {
    return "Turmeric WASM " TURMERIC_VERSION;
}

/* ---------------------------------------------------------------------------
 * Multi-expression evaluation helpers
 * ---------------------------------------------------------------------------
 */

/* Evaluate multiple expressions in sequence.
 * Useful for batch execution.
 * 
 * Arguments:
 *   inputs - Array of NUL-terminated Turmeric source strings
 *   count - Number of inputs in the array
 *   outputs - Pre-allocated array of char* with count elements
 *             Each element will be set to a malloc'd result string
 *             (or NULL on error for that expression)
 * 
 * Returns:
 *   The number of successfully evaluated expressions.
 *   Caller must free each non-NULL output string with turi_wasm_free().
 */
int turi_wasm_eval_batch(const char **inputs, int count, char **outputs) {
    if (!g_env) return 0;
    if (!inputs || count <= 0 || !outputs) return 0;
    
    int success_count = 0;
    for (int i = 0; i < count; i++) {
        outputs[i] = NULL;
        if (!inputs[i]) {
            outputs[i] = turi_wasm_strdup("Error: NULL input");
            continue;
        }
        
        TuriValue result = turi_eval(g_env, inputs[i]);
        wasm_flush_program_output();
        char buf[2048];
        turi_value_repr(buf, sizeof(buf), result);
        outputs[i] = turi_wasm_strdup(buf);
        
        if (!turi_is_error(result)) {
            success_count++;
        }
    }
    
    return success_count;
}

/* Format a Turmeric source string.
 * Returns a malloc'd formatted string, or NULL on parse error.
 * Caller must free with turi_wasm_free_string(). */
char *turi_wasm_format(const char *input) {
    if (!input) return NULL;

    size_t input_len = strlen(input);

    SourceFile file = {0};
    file.path        = "<format>";
    file.src         = input;
    file.len         = input_len;
    file.file_id     = 0;
    file.reader_type = READER_TURMERIC;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    char *result = NULL;

    if (forms && !diag_had_error()) {
        FmtOptions opts = {0};
        opts.indent_width = 2;
        opts.line_width   = 80;
        opts.src          = input;
        opts.src_len      = input_len;

        Buf out;
        buf_init(&out);
        if (fmt_print(&out, forms, nforms, opts) == 0) {
            result = turi_wasm_strdup(out.data ? out.data : "");
        }
        buf_free(&out);
    }

    symtab_free(&st);
    arena_free(&arena);
    /* Reset error state so subsequent eval calls are not poisoned */
    diag_reset();

    return result;
}

/* Shutdown the Turmeric WASM runtime.
 * Frees all resources. Call turi_wasm_init() again to restart. */
void turi_wasm_shutdown(void) {
    if (g_env) {
        turi_env_free(g_env);
        g_env = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Reader language mode (Phase S2)
 * ---------------------------------------------------------------------------
 */

/* Set the reader language mode for subsequent evaluations.
 *
 * `name` is the `#lang` directive tail -- a base dialect name and nothing
 * else (e.g. "turmeric/sweet", "saffron"), exactly the text after `#lang `.
 * Builds a synthetic "#lang <name>" line and runs it through
 * detect_lang_dialect so the same validation logic used by the REPL and the
 * interpreter applies here too.
 *
 * A base change resets the session -- losing session state on a language
 * switch is the already-accepted behaviour, and the UI warns before it
 * happens.
 *
 * Returns 0 on success, 1 if the base is unknown or anything trails it. */
int turi_wasm_set_lang(const char *name) {
    if (!g_env || !name) return 1;

    /* Build a synthetic "#lang <name>" to reuse detect_lang_dialect's
     * validation. */
    char buf[256];
    int written = snprintf(buf, sizeof(buf), "#lang %s", name);
    if (written < 0 || (size_t)written >= sizeof(buf)) return 1;

    const char  *rest;
    size_t       rest_len;
    const char  *bad     = NULL;
    size_t       bad_len = 0;
    /* saffron-lang-plan S8: detect_lang_DIALECT, not the plain detect_lang.
     * The latter reports only the reader, so every Saffron base came
     * back as its reader half (`saffron` -> READER_TURMERIC) with the language
     * silently dropped -- the picker would have "switched" to Saffron and left
     * the session in Turmeric. */
    LangDialect dialect = LANG_TURMERIC;
    ReaderType rt = detect_lang_dialect(buf, (size_t)written,
                                        &rest, &rest_len,
                                        &bad, &bad_len,
                                        &dialect);

    /* rest == buf means no #lang was recognised (pointer unchanged). */
    if (rest == buf || rt == READER_UNKNOWN || rt == (ReaderType)-1) return 1;
    /* A trailing token is a hard reject, matching TUR-E0330 on the compiled
     * path -- silently ignoring it would leave the UI and the environment
     * disagreeing.  (A RETIRED layer token does not reach here: the detector
     * accepts and consumes it, so a pasted `#lang turmeric stringed` header
     * still selects the turmeric base rather than failing the switch.) */
    if (bad) return 1;

    /* r7rs-lang-plan R9: a language with a different PRELUDE cannot be a
     * rewind to the pinned preload, which holds the old language's -- it
     * needs a fresh session with the new dialect seeded before the preload,
     * as the native REPL builds one (repl_fresh_env in src/turi/repl.c).
     * Picking r7rs used to keep Turmeric's preload, so `(display ...)` met
     * Turmeric's `display` method. */
    if (lang_traits(dialect)->prelude != lang_traits(g_env->lang)->prelude) {
        turi_env_free(g_env);
        g_env = turi_env_new();
        if (!g_env) return 1;
        turi_env_set_diag_sink(g_env, wasm_diag_sink, g_env);
        g_env->lang        = dialect;
        g_env->reader_type = rt;
        g_lang_prelude     = lang_traits(dialect)->prelude;
        wasm_preload_stdlib(g_env);
        return 0;
    }
    /* Full switch (no-op when nothing changes): keeps the pinned stdlib
     * preload across an explicit UI language switch, for the same reason the
     * inline `#lang` path does (web-repl-lang-switch-drops-stdlib). */
    turi_env_apply_lang_dialect(g_env, rt, dialect);
    return 0;
}

/* Return the current `#lang` BASE as a static string -- both axes, not just
 * the reader.  Reporting `reader_type_name` alone answered "turmeric" for a
 * Saffron session, since every Saffron base shares a Turmeric reader. */
const char *turi_wasm_get_lang(void) {
    static char base[64];
    if (!g_env) return reader_type_name(READER_TURMERIC);
    lang_base_spelling_of(g_env->lang, g_env->reader_type, base, sizeof base);
    return base;
}

/* ---------------------------------------------------------------------------
 * Language registry export (try-turmeric-lang-toggle-plan T1)
 * ---------------------------------------------------------------------------
 */

/* Human-readable labels for the READER axis, keyed by the unqualified reader
 * suffix lang_base_at reports -- not by the full base token.
 *
 * This used to be a table of whole base names, and it drifted the moment the
 * LANGUAGE axis arrived: `saffron`, `saffron/curly-infix`, `saffron/neoteric`
 * and `saffron/sweet` were all spellable and all worked when typed, but the
 * picker still offered the original four, so the dialect was unreachable from
 * the UI.  Keying on the reader suffix means a new LANGUAGE costs this table
 * nothing -- the base list itself is walked from lang_base_at, the same
 * cross-product `tur dialects` prints.  The legacy `sweet-exp` alias is
 * accepted on input but deliberately not offered. */
static const struct {
    const char *reader;
    const char *label;
} WASM_READER_LABELS[] = {
    { "s-expr",      "S-expression" },
    { "curly-infix", "Curly-infix" },
    { "neoteric",    "Neoteric" },
    { "sweet",       "Sweet-expression" },
    { "scheme",      "Scheme" },          /* r7rs-lang-plan R1: READER_R7RS */
};

static const char *wasm_reader_label(const char *reader) {
    size_t n = sizeof(WASM_READER_LABELS) / sizeof(WASM_READER_LABELS[0]);
    for (size_t i = 0; i < n; i++)
        if (strcmp(WASM_READER_LABELS[i].reader, reader) == 0)
            return WASM_READER_LABELS[i].label;
    return reader;
}

/* Append `s` to `b` as a JSON string body (no surrounding quotes). */
static void wasm_json_escape(Buf *b, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\\') {
            buf_putc(b, '\\');
            buf_putc(b, c);
        } else if ((unsigned char)c < 0x20) {
            buf_printf(b, "\\u%04x", (unsigned char)c);
        } else {
            buf_putc(b, c);
        }
    }
}

/* Return the `#lang` registry as JSON:
 *
 *   {"bases":[{"name":"turmeric","label":"S-expression",
 *              "language":"turmeric","reader":"s-expr",
 *              "experiment":null},...]}
 *
 * One axis, because `#lang` has one: bases are walked live from lang_base_at,
 * the same accessor that backs `tur dialects`, so the playground picker and
 * the CLI listing cannot disagree.  There used to be a second `"layers"` key;
 * the axis was decommissioned and the key is gone rather than emptied, so a
 * consumer cannot render a control for it.
 *
 * The returned string is built once and owned by this module; the caller
 * must NOT free it. */
const char *turi_wasm_lang_registry(void) {
    static char *cached = NULL;
    if (cached) return cached;

    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"bases\":[");
    size_t nbases = lang_bases_count();
    for (size_t i = 0; i < nbases; i++) {
        LangBaseDescriptor d;
        if (!lang_base_at(i, &d)) continue;
        if (i) buf_putc(&b, ',');
        buf_puts(&b, "{\"name\":\"");
        wasm_json_escape(&b, d.base);
        buf_puts(&b, "\",\"label\":\"");
        wasm_json_escape(&b, wasm_reader_label(d.reader));
        buf_puts(&b, "\",\"language\":\"");
        wasm_json_escape(&b, d.language);
        /* The READER half, unqualified (`s-expr`, `sweet`, `scheme`), the same
         * token `tur dialects` prints.  The picker groups by language and shows
         * one row per reader, so it needs the axis by name rather than having to
         * re-derive it from the base token. */
        buf_puts(&b, "\",\"reader\":\"");
        wasm_json_escape(&b, d.reader);
        /* null for a stable base; the gating EXPERIMENTS[] name for a gated
         * one (`r7rs` since r7rs-lang-plan R1).  A base with an experiment is
         * BADGED, never hidden, because the `#lang` line is itself the enable
         * and the row stays selectable. */
        buf_puts(&b, "\",\"experiment\":");
        if (d.experiment) {
            buf_puts(&b, "\"");
            wasm_json_escape(&b, d.experiment);
            buf_puts(&b, "\"");
        } else {
            buf_puts(&b, "null");
        }
        buf_puts(&b, "}");
    }
    buf_puts(&b, "]}");
    buf_putc(&b, '\0');   /* Buf does not NUL-terminate; strdup needs it */

    cached = turi_wasm_strdup(b.data ? b.data : "{\"bases\":[]}");
    buf_free(&b);
    return cached;
}

/* ---------------------------------------------------------------------------
 * Doc lookup bridge (D6: autodoc integration)
 * ---------------------------------------------------------------------------
 */

/* Look up documentation for a Turmeric name: a builtin or special form first,
 * then the stdlib's generated docstring table.
 *
 * Returns a pointer to a string owned by those tables, or NULL if the name is
 * not found.  The caller must NOT free it.
 *
 * This is a read-only query and must stay one: it reads the table from C and
 * never evaluates anything.  It used to evaluate `(doc-lookup "name")` into the
 * session -- which, since doc-lookup was never defined here, failed, and every
 * Run after a single doc-panel hover then failed too
 * (doc-lookup-poisons-the-playground-eval-session, PS2).
 *
 * In WASM builds this function is kept alive by EMSCRIPTEN_KEEPALIVE so that
 * JavaScript can call it via Module.ccall / Module._turi_doc_lookup.
 */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_doc_lookup(const char *name) {
    if (!g_env || !name) return NULL;

    /* The Emscripten link embeds stdlib/ at /stdlib, the module's cwd is "/",
     * so this is the same file `tur doc` reads from an install. */
    return tur_doc_prefer(
        name, tur_docstring_lookup_in(WASM_STDLIB_ROOT "/docstrings.tur", name));
}

static void wasm_diag_sink(struct TuriEnv *env, int level, const char *code,
                           const char *file, uint32_t line,
                           uint32_t col_start, uint32_t col_end,
                           const char *message, void *ud) {
    (void)env; (void)file; (void)line; (void)col_start; (void)col_end; (void)ud;

    /* Capture first TUR-E#### or TUR-W#### code into the REPL's slot */
    const char *last = turi_repl_get_last_diag_code();
    if ((!last || last[0] == '\0') && code && (strncmp(code, "TUR-E", 5) == 0 || strncmp(code, "TUR-W", 5) == 0)) {
        turi_repl_set_last_diag_code(code);
    }

    /* Print diagnostic to stderr so it shows up in Web Console or printErr */
    const char *lvl_str = (level == 0) ? "error" : (level == 1) ? "warning" : (level == 2) ? "note" : "help";
    if (file && file[0] != '\0') {
        if (code && code[0] != '\0') {
            fprintf(stderr, "%s:%u:%u: %s [%s]: %s\n", file, line, col_start, lvl_str, code, message);
        } else {
            fprintf(stderr, "%s:%u:%u: %s: %s\n", file, line, col_start, lvl_str, message);
        }
    } else {
        if (code && code[0] != '\0') {
            fprintf(stderr, "%s [%s]: %s\n", lvl_str, code, message);
        } else {
            fprintf(stderr, "%s: %s\n", lvl_str, message);
        }
    }
}

static void type_of_diag_sink(struct TuriEnv *env, int level, const char *code,
                              const char *file, uint32_t line,
                              uint32_t col_start, uint32_t col_end,
                              const char *message, void *ud) {
    (void)env; (void)code; (void)file; (void)line; (void)col_start; (void)col_end;
    char *buf = (char *)ud;
    if (level == 0 && buf[0] == '\0') {
        snprintf(buf, 2048, "error: %s", message);
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_type_of(const char *expr_src) {
    static char static_buf[2048];
    static_buf[0] = '\0';

    if (!g_env || !expr_src) return "";

    /* Install temporary diagnostic sink to capture type-elaboration errors */
    turi_env_set_diag_sink(g_env, type_of_diag_sink, static_buf);
    diag_reset();

    /* Build combined source so previous definitions are visible */
    Buf combined;
    buf_init(&combined);
    if (g_env->src_acc.len > 0) {
        buf_write(&combined, g_env->src_acc.data, g_env->src_acc.len);
    }
    buf_puts(&combined, expr_src);
    buf_putc(&combined, '\0');

    Arena    arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    SourceFile sfile = {0};
    sfile.path        = "<type>";
    sfile.src         = combined.data;
    sfile.len         = combined.len - 1;
    sfile.file_id     = 0;
    sfile.reader_type = g_env->reader_type;
    diag_register_file(&sfile);

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &sfile,
                                          g_env->reader_macros, &nforms);
    if (!forms || diag_had_error() || static_buf[0] != '\0') {
        goto cleanup;
    }

    Expr *prog = elaborate_program(&arena, &st, forms, nforms,
                                   /*stdlib_prefix=*/0, ".",
                                   /*separate_compilation=*/false,
                                   /*sandboxed=*/false,
                                   /*tc_env=*/NULL,
                                   /*include_dirs=*/NULL,
                                   /*n_include_dirs=*/0,
                                   /*out_n_fsd=*/NULL,
                                   g_env->reader_macros);
    if (!prog || diag_had_error() || static_buf[0] != '\0') {
        goto cleanup;
    }

    uint32_t n = prog->as.program.n;
    if (n == 0) {
        snprintf(static_buf, sizeof(static_buf), "empty expression");
    } else {
        Expr *last = prog->as.program.items[n - 1];
        snprintf(static_buf, sizeof(static_buf), "%s", type_name(last->type));
    }

cleanup:
    /* Restore diagnostic sink to default WASM sink */
    turi_env_set_diag_sink(g_env, wasm_diag_sink, g_env);

    arena_free(&arena);
    buf_free(&combined);

    return static_buf;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_explain(const char *code_or_null) {
    static char static_buf[8192];
    static_buf[0] = '\0';

    const char *code = code_or_null;
    if (!code || code[0] == '\0') {
        code = turi_repl_get_last_diag_code();
    }

    if (!code || code[0] == '\0') {
        return ":explain -- no recent diagnostic to explain. Try :explain TUR-E#### for a specific code.";
    }

    /* Normalise code to upper case */
    char norm_code[16];
    size_t len = strlen(code);
    if (len >= sizeof(norm_code)) {
        snprintf(static_buf, sizeof(static_buf), "unknown diagnostic code '%s'", code);
        return static_buf;
    }
    for (size_t i = 0; i < len; i++) {
        char c = code[i];
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        norm_code[i] = c;
    }
    norm_code[len] = '\0';

    if (diag_looks_like_code(norm_code)) {
        DiagCode dc = diag_code_from_string(norm_code);
        if (dc != DIAG_CODE_NONE) {
            FILE *f = tmpfile();
            if (f) {
                if (diag_explain(dc, f)) {
                    rewind(f);
                    size_t bytes = fread(static_buf, 1, sizeof(static_buf) - 1, f);
                    static_buf[bytes] = '\0';
                } else {
                    snprintf(static_buf, sizeof(static_buf), "unknown diagnostic code '%s'", norm_code);
                }
                fclose(f);
            } else {
                snprintf(static_buf, sizeof(static_buf), "unknown diagnostic code '%s'", norm_code);
            }
        } else {
            snprintf(static_buf, sizeof(static_buf), "unknown diagnostic code '%s'", norm_code);
        }
    } else {
        snprintf(static_buf, sizeof(static_buf), "unknown diagnostic code '%s'", code);
    }

    return static_buf;
}

/* ---------------------------------------------------------------------------
 * Time-travel tracer bridge (docs/archive/try-turmeric-tracer-plan.md, T3)
 *
 * The recorder, the reader and the replay are all in turi/trace.c, which has
 * compiled into this module since it landed -- it was simply dead-stripped,
 * because nothing in -sEXPORTED_FUNCTIONS reached it.  This section is the
 * reach.
 *
 * The page never decodes a .turtrace byte.  A recording stays in wasm memory
 * and the timeline asks turi_trace_replay_* the same questions `tur dap`'s
 * stepBack asks, so there is one decoder for the format rather than a C one
 * and a JavaScript one drifting apart.
 * ---------------------------------------------------------------------------
 */

#include "turi/trace.h"

/* A recording outlives the run that produced it: the replay reads straight out
 * of the trace's buffer, so the TurTrace is stopped but not ended until the
 * next Trace press. */
static TurTrace       *g_trace  = NULL;
static TurTraceReplay *g_replay = NULL;

/* The line in the session's accumulated blob at which THIS run's source began.
 *
 * The browser env accumulates every eval into env->src_acc and hands the whole
 * blob to the reader, so an interpreter line number is absolute in that blob,
 * not relative to what is in the editor -- which is why a five-line tab
 * reports its errors at `<eval>:75`.  A timeline that lit up line 75 of a
 * five-line file would be worse than no timeline, so the base is captured
 * before the run and the page subtracts it. */
static uint32_t g_trace_base_line = 1;

/* Lines already accumulated, +1: where the next appended chunk starts. */
static uint32_t wasm_acc_base_line(void) {
    if (!g_env) return 1;
    if (g_env->acc_next_line > 0) return g_env->acc_next_line;
    uint32_t line = 1;
    for (size_t i = 0; i < g_env->src_acc.len; i++)
        if (g_env->src_acc.data[i] == '\n') line++;
    /* turi_eval_typed joins the accumulated blob and the new chunk with a
     * newline (eval.c: `if (env->src_acc.len > 0) buf_putc(&env->src_acc, '\n')`),
     * so a non-empty accumulator costs one more line than it contains. */
    if (g_env->src_acc.len > 0) line++;
    return line;
}

static void wasm_trace_clear(void) {
    if (g_replay) { turi_trace_replay_free(g_replay); g_replay = NULL; }
    if (g_trace)  { turi_trace_end(g_trace); g_trace = NULL; }
}

/* Record a program.  `has_main` comes from the page, which already decides the
 * same question for Run (definesMainEntry in web/main.js): a program defining
 * a top-level `main` loads its forms and then runs `(main)`.  Keeping the rule
 * on one side means Run and Trace cannot disagree about where a program
 * starts.
 *
 * Returns the number of recorded steps, -1 if the recorder could not attach or
 * the program failed to load, and -2 when `has_main` was claimed and no `main`
 * closure materialized.
 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int turi_wasm_trace_run(const char *input, uint32_t max_steps, int has_main) {
    if (!g_env || !input) return -1;
    wasm_trace_clear();
    turi_repl_set_last_diag_code("");
    g_trace_base_line = wasm_acc_base_line();

    /* The order is cmd_trace's (src/driver/cmd_eval.c) by way of cmd_eval_h: attach
     * the debugger, install the recorder on it, load the program UNARMED so
     * the recording is of the program and not of its definitions, then arm and
     * call main. */
    turi_debug_enable(g_env, NULL, NULL);

    TurTraceOpts opts;
    memset(&opts, 0, sizeof opts);
    opts.max_steps = max_steps;
    /* Kept on in the browser.  output_drain writes every captured byte back
     * out to the saved descriptor as well (trace.c: "the tracer is a recorder,
     * not a muzzle"), so the console still streams live while the OUTPUT
     * records accumulate for the scrub. */
    opts.capture_output = true;
    /* The memset leaves grain at TUR_TRACE_GRAIN_NODE, which is what the
     * timeline wants: a browser scrub is the one client that can actually show
     * sub-expression movement, because it owns the editor and the site carries
     * a column range. */

    g_trace = turi_trace_begin(g_env, &opts);
    if (!g_trace) {
        turi_debug_disable(g_env);
        return -1;
    }

    int rc = 0;
    char type_tag[64] = {0};

    if (has_main) {
        TuriValue top = turi_eval_typed(g_env, input, type_tag, sizeof(type_tag));
        if (turi_is_error(top)) {
            rc = -1;
        } else {
            TuriValue main_fn = turi_env_get(g_env, "main");
            if (main_fn.tag == TURI_CLOSURE) {
                turi_debug_arm(g_env);
                (void)turi_call(g_env, main_fn, NULL, 0);
            } else {
                rc = -2;
            }
        }
    } else {
        /* No entry point, so the top-level forms are the program.  Arm first
         * or there is nothing to record. */
        turi_debug_arm(g_env);
        TuriValue top = turi_eval_typed(g_env, input, type_tag, sizeof(type_tag));
        if (turi_is_error(top)) rc = -1;
    }

    /* Not optional.  The recorder owns the env's pause handler and this module
     * keeps ONE env for the life of the tab -- a handler left installed turns
     * every later eval at the prompt into a traced one. */
    turi_trace_stop(g_trace);
    turi_debug_disable(g_env);
    wasm_flush_program_output();

    if (rc != 0) {
        wasm_trace_clear();
        return rc;
    }

    size_t len = 0;
    const uint8_t *bytes = turi_trace_bytes(g_trace, &len);
    if (bytes) g_replay = turi_trace_replay_open(bytes, len);
    if (!g_replay) {
        wasm_trace_clear();
        return -1;
    }
    return (int)turi_trace_replay_steps(g_replay);
}

/* Recording-level facts, as JSON.  `truncated` is the header flag: the step
 * cap ended the run, and the UI must say so rather than present a partial
 * recording as a whole one. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_wasm_trace_stats(void) {
    static char *cached = NULL;
    free(cached);
    cached = NULL;
    if (!g_trace || !g_replay) return "{\"steps\":0}";

    TurTraceStats st;
    memset(&st, 0, sizeof st);
    turi_trace_stats(g_trace, &st);
    size_t len = 0;
    (void)turi_trace_bytes(g_trace, &len);

    Buf b;
    buf_init(&b);
    buf_printf(&b,
        "{\"steps\":%u,\"enters\":%u,\"pops\":%u,\"changes\":%u,"
        "\"peakDepth\":%u,\"outputBytes\":%u,\"truncated\":%s,\"bytes\":%u,"
        "\"baseLine\":%u}",
        (unsigned)turi_trace_replay_steps(g_replay), (unsigned)st.enters,
        (unsigned)st.pops, (unsigned)st.changes, (unsigned)st.peak_depth,
        (unsigned)st.output_bytes, st.truncated ? "true" : "false",
        (unsigned)len, (unsigned)g_trace_base_line);
    buf_putc(&b, '\0');
    cached = turi_wasm_strdup(b.data ? b.data : "{\"steps\":0}");
    buf_free(&b);
    return cached;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
uint32_t turi_wasm_trace_seek(uint32_t index) {
    if (!g_replay) return 0;
    return turi_trace_replay_seek(g_replay, index);
}

/* The whole cursor as one JSON payload: index, the frame stack innermost
 * first, each frame's bindings, and the program output produced strictly
 * before this step.
 *
 * One message rather than four, because every one of these changes on every
 * seek and a scrub issues a seek per animation frame. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_wasm_trace_state(void) {
    static char *cached = NULL;
    free(cached);
    cached = NULL;
    if (!g_replay) return "{\"index\":0,\"steps\":0,\"frames\":[],\"output\":\"\"}";

    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"index\":%u,\"steps\":%u,\"frames\":[",
               (unsigned)turi_trace_replay_index(g_replay),
               (unsigned)turi_trace_replay_steps(g_replay));

    int nframes = turi_trace_replay_frame_count(g_replay);
    for (int i = 0; i < nframes; i++) {
        TurTraceFrame f;
        memset(&f, 0, sizeof f);
        if (!turi_trace_replay_frame_at(g_replay, i, &f)) continue;
        if (i) buf_putc(&b, ',');
        buf_puts(&b, "{\"fn\":\"");
        wasm_json_escape(&b, f.fn_name);
        buf_puts(&b, "\",\"file\":\"");
        wasm_json_escape(&b, f.file_path);
        /* endCol is exclusive, and 0 when the recording carries no range (a v1
         * file).  A client that wants to highlight the expression rather than
         * the line checks for 0 rather than assuming a range is there. */
        buf_printf(&b, "\",\"line\":%u,\"col\":%u,\"endCol\":%u,\"locals\":[",
                   (unsigned)f.line, (unsigned)f.col, (unsigned)f.col_end);
        int nlocals = turi_trace_replay_local_count(g_replay, i);
        for (int j = 0; j < nlocals; j++) {
            const char *name = NULL, *repr = NULL;
            if (!turi_trace_replay_local_at(g_replay, i, j, &name, &repr))
                continue;
            if (j) buf_putc(&b, ',');
            buf_puts(&b, "{\"name\":\"");
            wasm_json_escape(&b, name);
            buf_puts(&b, "\",\"repr\":\"");
            wasm_json_escape(&b, repr);
            buf_puts(&b, "\"}");
        }
        buf_puts(&b, "]}");
    }

    buf_puts(&b, "],\"output\":\"");
    size_t olen = 0;
    const char *out = turi_trace_replay_output(g_replay, &olen);
    if (out) {
        /* wasm_json_escape wants a NUL-terminated string and the output buffer
         * is a length-counted blob, so escape it by hand on the same rules. */
        for (size_t i = 0; i < olen; i++) {
            char c = out[i];
            if (c == '"' || c == '\\') { buf_putc(&b, '\\'); buf_putc(&b, c); }
            else if ((unsigned char)c < 0x20) buf_printf(&b, "\\u%04x", (unsigned char)c);
            else buf_putc(&b, c);
        }
    }
    buf_puts(&b, "\"}");
    buf_putc(&b, '\0');

    cached = turi_wasm_strdup(b.data ? b.data : "{\"index\":0,\"steps\":0,\"frames\":[],\"output\":\"\"}");
    buf_free(&b);
    return cached;
}

/* The site and depth of an arbitrary step, WITHOUT seeking.
 *
 * This is the one that makes a timeline affordable: drawing a depth ribbon or
 * marking which lines were ever executed asks this of thousands of indices,
 * and a seek rebuilds state from the start of the stream each time.  trace.h
 * spells out the consequence of confusing the two: "which is not slow, it is a
 * hang". */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_wasm_trace_site_at(uint32_t index) {
    static char *cached = NULL;
    free(cached);
    cached = NULL;
    if (!g_replay) return "{\"line\":0,\"depth\":0,\"file\":\"\"}";

    const char *file = NULL;
    uint32_t line = 0;
    bool ok = turi_trace_replay_site_at(g_replay, index, &file, &line);
    int depth = turi_trace_replay_depth_at(g_replay, index);

    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"file\":\"");
    if (ok) wasm_json_escape(&b, file);
    buf_printf(&b, "\",\"line\":%u,\"depth\":%d}", (unsigned)(ok ? line : 0), depth);
    buf_putc(&b, '\0');
    cached = turi_wasm_strdup(b.data ? b.data : "{\"line\":0,\"depth\":0,\"file\":\"\"}");
    buf_free(&b);
    return cached;
}

/* Scan for the next step on `line`, forwards (dir > 0) or backwards.  A miss
 * returns the boundary index with hit=false, which is what a continue with no
 * breakpoint ahead of it should do -- and what stops the UI presenting "ran to
 * the end" as "found it". */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_wasm_trace_find_line(int dir, const char *file, uint32_t line) {
    static char *cached = NULL;
    free(cached);
    cached = NULL;
    if (!g_replay) return "{\"index\":0,\"hit\":false}";

    bool hit = false;
    uint32_t idx = turi_trace_replay_find_line(g_replay, dir, file ? file : "",
                                               line, &hit);
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"index\":%u,\"hit\":%s}", (unsigned)idx,
               hit ? "true" : "false");
    buf_putc(&b, '\0');
    cached = turi_wasm_strdup(b.data ? b.data : "{\"index\":0,\"hit\":false}");
    buf_free(&b);
    return cached;
}

/* Every OUTPUT record in the recording, concatenated.
 *
 * turi_trace_replay_output answers "what had been printed by the cursor's
 * step", which is the right question everywhere except at the end of the run:
 * a program whose last act is a println drains it AFTER the final STEP, so the
 * trailing OUTPUT records sit past every stop point and the transcript at the
 * last step is empty.  Scrubbing to the end of `(println (fib 6))` and being
 * told nothing was printed reads as a broken timeline, so the page asks this
 * instead once the cursor is on the last step.
 *
 * Built on the public reader rather than by changing the replay, because the
 * replay's semantics are correct for its own question and `tur dap` depends on
 * them. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_wasm_trace_output_full(void) {
    static char *cached = NULL;
    free(cached);
    cached = NULL;
    if (!g_trace) return "";

    size_t len = 0;
    const uint8_t *bytes = turi_trace_bytes(g_trace, &len);
    TurTraceReader r;
    if (!bytes || !turi_trace_open(&r, bytes, len)) return "";

    Buf b;
    buf_init(&b);
    TurTraceRecord rec;
    while (turi_trace_next(&r, &rec)) {
        if (rec.tag != TUR_TRACE_OUTPUT || !rec.payload) continue;
        buf_write(&b, (const char *)rec.payload, rec.payload_len);
    }
    buf_putc(&b, '\0');
    cached = turi_wasm_strdup(b.data ? b.data : "");
    buf_free(&b);
    return cached;
}

/* The raw recording, so the page can offer it as a .turtrace download that
 * `tur trace --dump` reads.  Bytes, not a string: a NUL inside a rendered
 * value would truncate it. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const uint8_t *turi_wasm_trace_buffer(void) {
    if (!g_trace) return NULL;
    size_t len = 0;
    return turi_trace_bytes(g_trace, &len);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
uint32_t turi_wasm_trace_buffer_len(void) {
    if (!g_trace) return 0;
    size_t len = 0;
    (void)turi_trace_bytes(g_trace, &len);
    return (uint32_t)len;
}

/* Drop the recording.  Called when the timeline closes, so a megabyte of trace
 * does not sit in wasm memory for the rest of the session. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void turi_wasm_trace_release(void) {
    wasm_trace_clear();
}

/* ---------------------------------------------------------------------------
 * SX8c: the solver, answerable from the playground
 *
 * The refinement solver is already IN the wasm module -- `compiler/refine_*.c`
 * are `TUR_CORE_SOURCES`, so every browser session has been shipping S0..S3 all
 * along.  This makes it visible: the "static checking at zero download cost"
 * story is hard to believe from a page that cannot demonstrate it.
 *
 * The semantics are `tur smt`'s, deliberately and to the letter -- the same
 * session reader (so `(push)`/`(pop)` scope assertions and each `(check-sat)`
 * is answered where it appears), the same S0..S3 chain in the same order, the
 * same bounded model search, and the same "a script with no `(check-sat)` is
 * decided once at the end".  A window onto the solver has to show the same
 * solver from every angle; a browser build that answered differently from the
 * CLI would be a second solver wearing the first one's name.
 *
 * Read-only by construction, which is what SX8c's own spec asks for: nothing
 * here touches elaboration or discharge, so no answer from this API can elide
 * a runtime check.  Interrogation proposes; the C chain in a real compile
 * decides.
 * ---------------------------------------------------------------------------
 */

#include "compiler/refine_smtlib.h"
#include "compiler/refine_solver.h"

static void smt_json_str(Buf *out, const char *s) {
    buf_putc(out, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"':  buf_puts(out, "\\\""); break;
            case '\\': buf_puts(out, "\\\\"); break;
            case '\n': buf_puts(out, "\\n");  break;
            case '\r': buf_puts(out, "\\r");  break;
            case '\t': buf_puts(out, "\\t");  break;
            default:
                if (*p < 0x20) buf_printf(out, "\\u%04x", *p);
                else           buf_putc(out, (char)*p);
        }
    }
    buf_putc(out, '"');
}

static void smt_json_model(Buf *out, const RefineModel *m) {
    buf_puts(out, "[");
    for (uint32_t i = 0; m && i < m->n; i++) {
        const RefineModelBinding *b = &m->bindings[i];
        if (i) buf_puts(out, ",");
        buf_puts(out, "{\"name\":");
        smt_json_str(out, b->name);
        if (b->is_real)      buf_printf(out, ",\"sort\":\"Real\",\"value\":%g", b->rval);
        else if (b->is_bool) buf_printf(out, ",\"sort\":\"Bool\",\"value\":%s",
                                        b->ival ? "true" : "false");
        else                 buf_printf(out, ",\"sort\":\"Int\",\"value\":%lld",
                                        (long long)b->ival);
        buf_puts(out, "}");
    }
    buf_puts(out, "]");
}

/* One `(check-sat)`'s worth of work: run the chain, then the bounded search if
 * nothing proved it.  Mirrors the driver's `smt_answer`. */
static void smt_one_answer(RefineVC *vc, Arena *arena, Buf *out) {
    /* refine-chain-expands-the-same-dnf-four-times: the `_cc` stages, sharing
     * ONE run's cube set.  Each stage used to rebuild the same DNF from an
     * unchanged vc, so a query reaching S3 expanded it four times.  Same
     * verdicts by construction -- the set is identical, it is just built once.
     * The `refine_sN_decide` entry points still exist for a single-stage
     * caller; a chain has no use for them. */
    static const struct { const char *name;
                          RefineDecision (*fn)(RefineVC *, Arena *,
                                               RefineCubeCache *); } CHAIN[] = {
        { "S0 (trivial)",       refine_s0_decide_cc },
        { "S1 (EUF)",           refine_s1_decide_cc },
        { "S2 (arithmetic)",    refine_s2_decide_cc },
        { "S3 (Nelson-Oppen)",  refine_s3_decide_cc },
    };
    const char *decided_by = NULL;
    RefineVerdict v = RT_UNKNOWN;
    RefineCubeCache cc = {0};
    for (size_t i = 0; i < sizeof(CHAIN)/sizeof(CHAIN[0]); i++) {
        RefineDecision d = CHAIN[i].fn(vc, arena, &cc);
        if (d.verdict != RT_UNKNOWN) { v = d.verdict; decided_by = CHAIN[i].name; break; }
    }

    /* The goal is `false`, so VALID means the assertion set is UNSAT.  The
     * stages only ever prove; `sat` has to come from the bounded search, which
     * is the only thing in the solver allowed to answer INVALID -- and it does
     * so with a witness rather than a guess, which is why the model rides
     * along here instead of being a separate call. */
    buf_puts(out, "{\"answer\":");
    if (v == RT_VALID) {
        smt_json_str(out, "unsat");
    } else {
        RefineModel *m = refine_model_search(vc, arena);
        if (m) {
            smt_json_str(out, "sat");
            decided_by = "bounded model search";
            buf_puts(out, ",\"model\":");
            smt_json_model(out, m);
        } else {
            smt_json_str(out, "unknown");
        }
    }
    if (decided_by) {
        buf_puts(out, ",\"decided_by\":");
        smt_json_str(out, decided_by);
    }
    buf_puts(out, "}");
}

/* `Buf` carries a length and does NOT NUL-terminate -- it is written for
 * buf_to_file, where the length is the contract.  Handing `b->data` straight
 * back as a C string is therefore an overread, and a silent one: the JSON
 * parses fine and trailing heap garbage rides along behind the closing brace.
 * Every exit from turi_smt_check goes through here.  (`grow` is realloc-backed,
 * so the result is free()-able, which is what the JS side does.) */
static char *smt_finish(Buf *b) {
    buf_putc(b, '\0');
    return b->data ? b->data : turi_wasm_strdup("");
}

/* Answer an SMT-LIB2 script.  Returns a malloc'd JSON string the caller frees
 * with turi_wasm_free_string (JS: Module._free).
 *
 *   {"schema":1,"results":[{"answer":"unsat","decided_by":"S2 (arithmetic)"}]}
 *   {"schema":1,"results":[],"error":"outside the accepted fragment: ..."}
 *
 * `schema` is 1: the shape is stable since SX9, matching `--dump-refine=json`
 * (schema 0 was the same shape flagged unstable).
 * `results` carries one entry per `(check-sat)`, in script order.
 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
char *turi_smt_check(const char *smtlib) {
    Buf out; buf_init(&out);
    buf_puts(&out, "{\"schema\":1,\"results\":[");
    if (!smtlib) {
        buf_puts(&out, "],\"error\":\"no input\"}");
        return smt_finish(&out);
    }

    Arena arena;
    arena_init(&arena, 1 << 20);
    SmtlibSession *s = refine_smtlib_session_new(&arena);
    refine_smtlib_session_feed(s, smtlib, strlen(smtlib));

    const char *err = NULL;
    bool answered = false;
    for (bool done = false; !done; ) {
        switch (refine_smtlib_session_step(s)) {
            case SMT_EV_CHECK_SAT:
                if (answered) buf_puts(&out, ",");
                smt_one_answer(refine_smtlib_session_vc(s), &arena, &out);
                answered = true;
                break;
            case SMT_EV_ERROR:
                err = refine_smtlib_session_err(s);
                if (!err) err = "unsupported script";
                done = true;
                break;
            case SMT_EV_END:
            case SMT_EV_EXIT:
                done = true;
                break;
            /* `(get-model)` is a no-op here: the witness already rides along
             * with the `sat` it belongs to, so a caller never has to ask. */
            case SMT_EV_GET_MODEL:
            case SMT_EV_OK:
                break;
        }
    }

    /* A script that never asked is still decided once, exactly as `tur smt`
     * does it -- every corpus benchmark relies on that, and two doors onto one
     * solver should not disagree about what an unasked script means. */
    if (!err && !answered)
        smt_one_answer(refine_smtlib_session_vc(s), &arena, &out);

    buf_puts(&out, "]");
    if (err) {
        buf_puts(&out, ",\"error\":");
        smt_json_str(&out, err);
    }
    buf_puts(&out, "}");
    arena_free(&arena);
    return smt_finish(&out);
}
