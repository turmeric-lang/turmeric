/* tur: the Turmeric compiler driver's entry point.
 *
 * main() runs the driver on a stack sized for the compiler's recursion (the
 * note above TurMainArgs says why); tur_main_inner sets the process up, parses
 * the global flags and runs the subcommand the command table names.  The
 * driver itself lives in src/driver/: driver.h says how its files share
 * declarations, and driver/cli.c holds the command and flag tables. */

#include "driver/driver.h"
#include "driver/commands.h"

#ifdef _WIN32
#include <io.h>       /* _setmode, _fileno, _write */
#endif

#include "compiler/stack_guard.h" /* tur_run_on_big_stack -- see the note at EOF */
#include "global.h"               /* use_json_output */
#include "turi/jit_ffi.h"         /* jit-ffi-c2mir-plan: dynamic-FFI provider */

static int tur_main_inner(int argc, char **argv) {
    /* profiling-plan C0+C1: --timings first, so the clock starts before
     * anything it should measure, and stripped before g_main_argv is taken
     * below -- the split build's cache key hashes the command line, and a
     * profiler that changed the key would profile a cold cache. */
    drv_parse_timings_flag(&argc, argv);
    /* The representation and runtime switches the environment can flip. */
    drv_apply_env_overrides();

#ifdef _WIN32
    /*
     * Put stdout/stderr in binary mode so a newline stays one byte.
     *
     * Windows opens them in text mode by default, which silently rewrites every
     * '\n' to "\r\n" on the way out.  Three things here break on that, and two
     * of them break silently:
     *
     *   - `tur emit-c` writes generated C to stdout, so every line would gain a
     *     \r and stop matching the tests/fixtures/<name>/expected.c snapshots.
     *   - The LSP and DAP servers frame messages with a Content-Length byte
     *     count computed BEFORE the text-mode expansion -- so the count would
     *     understate the bytes actually written and desynchronise the protocol.
     *   - `tur fmt --stdout` would rewrite the file's line endings as a side
     *     effect of printing it.
     *
     * Binary mode makes output byte-exact, matching every other platform.
     */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    /* SN1: stash argv[0] for exe-path fallback in resolve_stdlib_root().
     * Platform APIs (_NSGetExecutablePath / /proc/self/exe) are tried
     * first; argv[0] is the last-resort path source. */
    g_argv0 = (argc > 0) ? argv[0] : NULL;
    g_main_argc = argc;
    g_main_argv = (const char *const *)argv;
    /* Resolve the stdlib root once at startup so TUR_STDLIB_DIR is
     * propagated into the process env before any subsystem (elaborator,
     * worker, interpreter) reads it. */
    (void)resolve_stdlib_root();

#ifdef TUR_HAVE_JIT
    /* jit-ffi-c2mir-plan: install the c2mir-backed dynamic-FFI provider so
     * the interpreter's extern-c registration, call-ptr routing, and the
     * spice FFI ladder can synthesize call thunks at runtime.  JIT builds
     * only; without it every consumer keeps the non-JIT fallback behavior. */
    tur_jit_ffi_install();
    /* aot-compiled-repl-plan C1: the interpreter's compiler for inline-C
     * defns it cannot run.  Inert under TUR_REPL_JIT_INLINE_C=0. */
    turi_set_inline_c_jit_hook(&g_repl_inline_c_jit_hook);
#endif

    /* Phase 8: the global flags, wherever they appear before `--`.  A flag
     * that fails (an unknown --enable=, say) has already said why. */
    int flags_rc = drv_parse_global_flags(&argc, argv);
    if (flags_rc) return flags_rc;
    /* The parse strips what it consumed and NULL-terminates the shorter argv;
     * a stale count let the split-library cache key (preamble_split.c) walk
     * past that NULL, so `tur run --enable=<x> prog.scm` crashed.  The
     * experiments it stripped are keyed there from their own globals. */
    g_main_argc = argc;

    /* Initialize diagnostics - use color unless --no-color or --json-diagnostics specified */
    /* JSON output disables color */
    if (use_json_output) g_json_diagnostics = true;
    diag_init(!g_no_color && !g_json_diagnostics && stderr_is_tty());
    diag_set_json_output(g_json_diagnostics);

    if (g_explain_code) return cmd_explain(g_explain_code);

    /* E2: --version / -V / -v */
    if (argc >= 2 && (strcmp(argv[1], "--version") == 0
                   || strcmp(argv[1], "-V") == 0
                   || strcmp(argv[1], "-v") == 0)) {
        printf("tur: the Turmeric compiler (v" TUR_VERSION ")\n");
        printf("Run `tur --help` for more commands.\n");
        return 0;
    }

    /* E1: --help / -h at top level */
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        drv_usage();
        return 0;
    }

    if (argc < 2) return drv_usage();

    /* The command table (driver/cli.c): an exact spelling, else a unique
     * prefix.  More than one prefix hit prints the usage. */
    bool ambiguous = false;
    const DrvCommand *c = drv_find_command(argv[1], &ambiguous);
    if (ambiguous) return drv_usage();
    if (c) return c->run(argc, argv);

    /* script-spice-deps-plan SD-D6: `tur report.tur a b` runs the script --
     * after the built-ins (which always win), before `tur-<cmd>` lookup. */
    if (drv_is_script_word(argv[1])) return drv_run_script(argc, argv);
    /* namespaced-spice-binaries D3: `tur alpha/foo` is installed spice
     * alpha's tur-foo.  An existing file of that name stays a script. */
    if (drv_qualified_bin_shaped(argv[1]))
        return drv_run_qualified_bin("tur", argv[1], argv + 2, argc - 2);
    if (drv_script_word_shaped(argv[1])) {
        fprintf(stderr, "tur: no such file '%s' (and no command of that name). "
                        "See 'tur --help'.\n", argv[1]);
        return 2;
    }

    /* GS-M2: subcommand fallthrough -- `tur foo bar` execs `tur-foo bar`
     * from $PATH when "foo" isn't a built-in. Built-ins always win.
     * If exec succeeds, this does not return. */
    return drv_try_external_subcommand(argc, argv);
}

/* ---------------------------------------------------------------------------
 * The compiler runs on a stack sized for its own recursion.
 *
 * Every phase that walks the AST -- reader, elaborator, emitter -- recurses
 * once per level of expression nesting, and how deep that goes is a property
 * of the INPUT.  A macro that expands into a nested chain can want more levels
 * than a default 8 MiB thread stack holds, and on a Debug+ASan build each
 * frame is ~40x its usual size.
 *
 * Rationing it with a per-phase depth constant was tried and failed in both
 * directions: the constant stopped being a ceiling as soon as a frame grew (an
 * ASan stack-overflow abort where a diagnostic should have printed), and it
 * rejected legitimate macro-generated code that no hand-written source would
 * reach.  See docs/archive/emit-depth-guard-loses-race-with-asan-stack.md.
 *
 * The emitter is where that surfaced, but it is not the only offender: an
 * ~400-level nested expression aborts in elab_call -> elab_form, which has no
 * depth guard at all.  So the stack is sized once, underneath the whole
 * driver, rather than per phase.  TUR_STACK_MB tunes it; jit_engine.c has done
 * the same for a JIT'd program's entry stack (TUR_JIT_STACK_MB) all along.
 *
 * emit_program and friends ALSO trampoline (emit_module.c).  That is not
 * redundant: tur_run_on_big_stack is re-entrant-safe, so it is a no-op when
 * reached from here, and it keeps the guarantee for an embedder that links
 * libturi and calls the emitter without going through this main.
 * ------------------------------------------------------------------------ */

typedef struct { int argc; char **argv; } TurMainArgs;

static int tur_main_job(void *p) {
    TurMainArgs *a = (TurMainArgs *)p;
    return tur_main_inner(a->argc, a->argv);
}

/* r7rs-lang-plan T5: `#lang r7rs`'s call/cc copies the C stack under
 * `tur --interpret` (src/turi/interpreter_natives.c).  ASan's use-after-return
 * detection moves address-taken locals onto a heap "fake stack" that a copy of
 * the real stack cannot see, so a sanitized `tur` runs with it off -- the
 * same default the prelude sets for a sanitized compiled Scheme program.
 * ASAN_OPTIONS still overrides it. */
#if defined(__SANITIZE_ADDRESS__)
const char *__asan_default_options(void);
const char *__asan_default_options(void) { return "detect_stack_use_after_return=0"; }
#endif

#ifdef _WIN32
/* A crash in tur.exe is SILENT on Windows: an access violation ends the
 * process with status 0xC0000005 and nothing on stderr, where POSIX at least
 * reports the signal.  That is how `tur mcp` came to die mid-session on CI
 * three times with no evidence at all
 * (docs/archive/mcp-server-exits-mid-session-on-windows.md).
 *
 * Name the exception and where it happened, as an offset into tur.exe --
 * `addr2line -e tur.exe <ImageBase + offset>` resolves it against the same
 * build (ImageBase from `objdump -p`, 0x140000000 by default) -- then carry on
 * exactly as before: EXCEPTION_CONTINUE_SEARCH leaves the default handling --
 * and so the exit status -- untouched.  Only an exception nothing else
 * handled reaches this, so it prints only on a process that is already dying.
 * Kept to a stack buffer and _write: after a stack overflow there is very
 * little stack left to report with. */
static LONG WINAPI tur_win_report_crash(EXCEPTION_POINTERS *ep) {
    char msg[160];
    uintptr_t at   = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    int n = snprintf(msg, sizeof msg,
                     "tur: fatal exception 0x%08lx at %p (tur.exe+0x%llx)\n",
                     (unsigned long)ep->ExceptionRecord->ExceptionCode,
                     (void *)at, (unsigned long long)(at - base));
    if (n > 0) (void)_write(2, msg, (unsigned)n);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(tur_win_report_crash);
#endif
    TurMainArgs a = { argc, argv };
    return tur_run_on_big_stack(tur_main_job, &a);
}
