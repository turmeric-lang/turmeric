/* wasm_jit_glue.c -- run a Turmeric program the way `tur jit` does, in the
 * browser (wasm-jit-plan P3).
 *
 * The page's Compile engine: the program goes through the driver's front half
 * (tur_front: compile_to_c, include hoisting, the autolink scan, the S2
 * preamble split, the pruner) and into the MIR engine, which on this target
 * interprets c2mir's output on the calling thread and resolves imports from
 * the generated table (tools/wasm-jit/gen-symtab.py).  This is cmd_jit
 * (src/driver/jit.c) without the command line, and with the interpreter as
 * the fallback where the native command falls back to cc (plan 5.7).
 *
 * The answer is a JSON object the page shows as its engine line:
 *
 *   {"engine":"compiled","exit":0,"front_ms":..,"compile_ms":..,"run_ms":..}
 *       the program ran on the engine; exit is its exit status
 *   {"engine":"interpreted","exit":0,"reason":"..."}
 *       the engine could not take the program (reason says why) and the
 *       interpreter ran it instead, in a fresh environment; exit is 1 when
 *       the interpreter reported an error
 *   {"engine":"error","exit":1,"reason":"..."}
 *       the program does not compile; the diagnostics went to stderr, and
 *       the interpreter is not tried, since it would report the same thing
 *
 * The program's own output goes where every other run's does: stdout and
 * stderr, which the page receives through Module.print / printErr.
 *
 * Only the engine's refusals fall back -- a c2mir rejection, an import this
 * target refuses (threads), a library it cannot load -- and those all happen
 * before main, so nothing has been printed twice.  A program that started and
 * then failed reports its exit status like any other.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define TUR_WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define TUR_WASM_EXPORT
#endif

#include "buf.h"
#include "driver/driver.h"
#include "jit_engine.h"
#include "compiler/jit_prune.h"
#include "compiler/refine_discharge.h"
#include "runtime/globals.h"   /* g_interpret_mode */
#include "wasm_glue.h"

/* Where c2mir finds headers: the musl subset (tools/wasm-jit/headers.txt and
 * its stdatomic.h), runtime/hamt.h and turi/r7rs_embed.h, as the module
 * embeds them. */
static const char *const k_include_dirs[] = { "/sysinclude", "/src/runtime", "/src" };

static double glue_now_ms(void) {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return 0.0;
#endif
}

/* Append `s` to `b` as a JSON string body (quotes added by the caller). */
static void json_escape(Buf *b, const char *s) {
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') buf_printf(b, "\\%c", c);
        else if (c == '\n') buf_puts(b, "\\n");
        else if (c < 0x20) buf_printf(b, "\\u%04x", c);
        else buf_printf(b, "%c", c);
    }
}

static char *result_json(const char *engine, int exit_code, const char *reason,
                         double front_ms, double compile_ms, double run_ms) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"engine\":\"%s\",\"exit\":%d", engine, exit_code);
    if (reason && *reason) {
        buf_puts(&b, ",\"reason\":\"");
        json_escape(&b, reason);
        buf_puts(&b, "\"");
    }
    if (front_ms > 0) buf_printf(&b, ",\"front_ms\":%.1f", front_ms);
    if (compile_ms > 0) buf_printf(&b, ",\"compile_ms\":%.1f", compile_ms);
    if (run_ms > 0) buf_printf(&b, ",\"run_ms\":%.1f", run_ms);
    buf_puts(&b, "}");
    return b.data;   /* malloc'd; the caller frees with turi_wasm_free_string */
}

/* The fallback: the interpreter runs the program, as `tur --interpret`
 * would (main included), on a fresh environment. */
static char *run_interpreted(const char *path, const char *reason) {
    fprintf(stderr, "tur: the page's compiler cannot run this program (%s); "
                    "running it on the interpreter instead\n", reason);
    fflush(stderr);
    int rc = turi_wasm_run_program(path);
    fflush(stdout);
    fflush(stderr);
    return result_json("interpreted", rc, reason, 0, 0, 0);
}

/* Split `args` (newline-separated) into argv[1..]; argv[0] is the program's
 * name.  Returns argc; *storage owns the strings. */
static int split_args(const char *args, char ***argv_out, char **storage) {
    size_t n = 1;
    for (const char *p = args; p && *p; p++) if (*p == '\n') n++;
    char **argv = calloc(n + 2, sizeof *argv);
    char *copy = strdup(args ? args : "");
    int argc = 0;
    argv[argc++] = "main";
    if (*copy) {
        char *save = NULL;
        for (char *tok = strtok_r(copy, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save))
            argv[argc++] = tok;
    }
    argv[argc] = NULL;
    *argv_out = argv;
    *storage = copy;
    return argc;
}

TUR_WASM_EXPORT
char *turi_wasm_jit_run(const char *src, const char *args) {
    if (!src) return result_json("error", 1, "no program", 0, 0, 0);
    if (turi_wasm_init() != 0) return result_json("error", 1, "the runtime did not start", 0, 0, 0);

    double t0 = glue_now_ms();
    mkdir("/tmp", 0777);
    const char *path = "/tmp/main.tur";
    FILE *f = fopen(path, "wb");
    if (!f) return result_json("error", 1, "cannot write the program to /tmp", 0, 0, 0);
    fputs(src, f);
    fclose(f);

    /* As repl_jit_build does (src/driver/jit.c): the module's interpreter
     * environment leaves g_interpret_mode on, and under it the emitter takes
     * the interpreter's branches and evicts the CPS backend's handlers. */
    refine_discharge_reset();
    bool saved_interp = g_interpret_mode;
    g_interpret_mode = false;
    g_emit_for_link = true;
    g_emit_for_wasm = false;   /* the runtime is resident in this module */
    Buf csrc;
    buf_init(&csrc);
    int rc = compile_to_c(path, &csrc, NULL, 0, NULL, 0);
    g_interpret_mode = saved_interp;
    if (rc != 0) {
        buf_free(&csrc);
        fflush(stderr);
        return result_json("error", 1, "the program does not compile", 0, 0, 0);
    }
    hoist_tur_include_directives(&csrc);
    Buf autolink;
    buf_init(&autolink);
    if (!scan_autolink_markers(&csrc, &autolink)) {
        buf_free(&csrc);
        buf_free(&autolink);
        return run_interpreted(path, "its autolink markers are malformed");
    }

    /* The S2 split swaps the fixed preamble for the resident runtime's
     * declarations; the pruner then drops what the program does not reach.
     * There is no full-unit retry here (TUR-W0071 natively): the full unit
     * embeds runtime sources that include <stdatomic.h>, which c2mir lacks,
     * so on this target the retry can only fail. */
    Buf reduced;
    buf_init(&reduced);
    JitPruneStats ps;
    memset(&ps, 0, sizeof ps);
    bool split = jit_try_split_preamble(&csrc, &reduced);
    if (split) {
        jit_prune_split_source(&reduced, &ps);
    } else {
        buf_write(&reduced, csrc.data, csrc.len);
        if (!jit_prune_full_source(&reduced, &ps)) {
            buf_free(&reduced);
            buf_init(&reduced);
            buf_write(&reduced, csrc.data, csrc.len);
        }
    }
    buf_free(&csrc);
    double front_ms = glue_now_ms() - t0;

    char **argv = NULL;
    char *arg_storage = NULL;
    int argc = split_args(args, &argv, &arg_storage);
    int prog_rc = 0;
    /* c2mir's warnings (unknown pragmas in musl's headers, empty structs)
     * are several lines per compile that say nothing about the program;
     * `tur run --engine=jit` drops them too.  TUR_JIT_WARNINGS in the
     * module's environment keeps them, for debugging the engine. */
    const char *warn = getenv("TUR_JIT_WARNINGS");
    tur_jit_set_quiet_warnings(!(warn && *warn));
    int jrc = tur_jit_execute(reduced.data, reduced.len,
                              autolink.len ? autolink.data : NULL,
                              (const char **)k_include_dirs,
                              (int)(sizeof k_include_dirs / sizeof k_include_dirs[0]),
                              argc, argv, &prog_rc);
    free(argv);
    free(arg_storage);
    buf_free(&reduced);
    buf_free(&autolink);
    fflush(stdout);
    fflush(stderr);

    double compile_ms = 0, run_ms = 0;
    tur_jit_last_timings(&compile_ms, &run_ms);
    if (jrc == TUR_JIT_OK)
        return result_json("compiled", prog_rc, NULL, front_ms, compile_ms, run_ms);

    const char *why = tur_jit_last_error();
    char reason[600];
    if (why && *why) snprintf(reason, sizeof reason, "%s", why);
    else if (jrc == TUR_JIT_ERR_COMPILE)
        snprintf(reason, sizeof reason, "c2mir rejected the generated C (see above)");
    else
        snprintf(reason, sizeof reason, "the engine could not start the program");
    return run_interpreted(path, reason);
}
