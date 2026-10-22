/* cli.c -- the command line: the per-subcommand usage text, `-I` parsing, the
 * `-X` deprecation and `--enable=` helpers, and the `--no-auto-*` flags.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

#include "prof.h"         /* --timings */
#include "global.h"       /* use_json_output */
#include "globals.h"
#include "cli/completion.h"
#include "cli/demangle.h"

/* cli-usage-error-paths-exit-zero: a `usage_*` helper prints the subcommand's
 * usage and returns 0 -- right for `--help`, which is a successful request
 * for help.  The ERROR paths (an unknown flag, a missing or duplicated input)
 * print the same text but must exit nonzero, or a script cannot tell a typo
 * from a successful build: `tur build --typo || exit 1` used to succeed.
 * They route through here.  2 is the subcommand-level usage status (`tur docs`
 * and `tur expand` already used it); 64 (EX_USAGE) stays with the top-level
 * dispatcher's unknown-subcommand path. */
int drv_usage_error(int (*usage)(void)) {
    (void)usage();
    return 2;
}

/* drop-x-flags-plan: every -X<name> below is an accept-and-warn no-op as
 * of v0.24.0.  The flags are still recognized for one full minor line so
 * downstream build.tur files keep compiling unchanged; each one emits
 * TUR-W0050 instead of enabling anything.  The features themselves are
 * unconditionally on (see src/runtime/globals.c). */
bool is_known_deprecated_x_flag(const char *s) {
    static const char *const names[] = {
        "-Xlinear", "-Xsubstructural", "-Xunique-types", "-Xgadt",
        "-Xunion-types", "-Xintersection-types", "-Xeffect-types",
        "-Xcontracts", "-Xsessions", "-Xdynamic-vars", "-Xcallcc",
        "-Xsized-types", "-Xdata-literals", "-Xjson-reader",
        "-Xschema-reader", "-Xsymbols",
    };
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        if (strcmp(s, names[k]) == 0) return true;
    }
    return false;
}

static void warn_deprecated_x_flag(const char *name) {
    fprintf(stderr, "warning [TUR-W0050]: %s is no longer needed; "
                    "the feature is on by default\n", name);
}

/* XF1 (experimental-flag-mechanism-plan): enable each comma-separated name in
 * a `--enable=<a,b,c>` value (or a manifest :experiments list).  An unknown
 * name is a hard error (TUR-E0310) so typos surface immediately.  Returns
 * true on success, false (after printing TUR-E0310) on the first unknown
 * name. `src` records the origin for the `tur experiments` source column. */
bool enable_experiment_list(const char *list, ExperimentSource src) {
    if (!list || !*list) return true;
    char copy[1024];
    strncpy(copy, list, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    char *tok = strtok(copy, ",");
    while (tok) {
        /* trim leading whitespace */
        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok) {
            if (!experiment_enable(tok, src)) {
                fprintf(stderr,
                        "error [TUR-E0310]: unknown experiment '%s'; "
                        "run 'tur experiments' for the list\n", tok);
                return false;
            }
        }
        tok = strtok(NULL, ",");
    }
    return true;
}

/* E1: per-subcommand help strings */
int usage_build(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur build [-I <dir>...] <file.tur> [-o <out>]   build a single file\n"
        "  tur build <dir> [-o <out>]                       build all .tur in dir\n"
        "  tur build --shared <dir> [-o <out>] [--manifest <p>]  build a shared library\n"
        "  tur compile [-I <dir>...] <file.tur> -o <out.o>  lower a .tur to an object + .link\n"
        "  tur link [--shared] <obj/src>... -o <out>        link objects (+ .link) into an exe/.so\n"
        "  tur emit-c [-I <dir>...] <file.tur>              emit C to stdout\n"
        "  tur emit-c [-I <dir>...] --build-dir <dir> <files...>  emit per-module .h/.c\n"
        "  tur emit-h [-I <dir>...] <file.tur>              emit header to stdout\n"
        "\n"
        "flags:\n"
        "  -o <out>          output file path\n"
        "  -I <dir>          add include directory for module resolution\n"
        "                    (repeat to add multiple; intra-spice imports usually\n"
        "                    want `-I src` from the spice root)\n"
        "  --release         compile at -O3 (the default is -O2; TUR_CC_FLAGS\n"
        "                    replaces either)\n"
        "  -B, --build-dir <d>  route generated .c/.h/.o + final artifact into <d>\n"
        "                    (subdirs: obj/, bin/, lib/). Defaults to\n"
        "                    <project-root>/build or <cwd>/build. Override layers:\n"
        "                    CLI flag > TUR_BUILD_DIR env > build.tur :build-dir.\n"
        "  --shared          build a shared library (`-fPIC -shared`, no main;\n"
        "                    Windows drops the -fPIC, which PE ignores anyway).\n"
        "                    Requires a directory argument. Exported defns are\n"
        "                    callable via dlopen/dlsym as `<module>__<name>`.\n"
        "                    Default output is <build-dir>/lib/lib<name>.so, or\n"
        "                    <name>.dll on Windows.\n"
        "  --split-build     build a single file as `compile` + `link` (cacheable\n"
        "                    `cc -c` object compiles + a link). Native builds only.\n"
        "  --no-split-build  force the monolithic single-`cc` build (the default).\n"
        "  --runtime=auto    (default) link the lean non-ASan libturt_runtime.a when\n"
        "                    locatable, else recompile the bare src/runtime autolink\n"
        "                    sources -- never links the ASan libturi.a on its own, so\n"
        "                    a default build matches the old source path.\n"
        "  --runtime=lib     force the archive link (lean preferred, else libturi.a).\n"
        "                    Set TUR_RUNTIME_LIB to point at the archive if not found.\n"
        "  --runtime=source  force recompiling the runtime sources.\n"
        "  --runtime=split   insist on the preamble split that auto takes on Linux\n"
        "                    and Windows when libturt_preamble.a is present: the\n"
        "                    fixed runtime preamble is linked, not recompiled per\n"
        "                    program.  TUR_PREAMBLE_SPLIT=0 keeps it inline.\n"
        "                    TUR_RUNTIME=auto|lib|source|split seeds the default.\n"
        "  --link-flags <f>  (tur link) extra linker flags, e.g. \"-L<dir> -lfoo\"\n"
        "  --manifest <p>    (with --shared) write exports.manifest to <p>\n"
        "                    (defaults to `<out>.manifest`). Lists each export\n"
        "                    as `<mod>/<defn> -> <mangled> :: (:args) -> :ret`.\n"
        "  --bundle-deps     (with --shared, on a project) link each :spices dep's\n"
        "                    modules into the library as well, so it loads with\n"
        "                    nothing else beside it; the manifest still lists only\n"
        "                    the project's own exports.\n"
        "  --target wasm     compile to WebAssembly via emcc (requires Emscripten)\n"
        "  --debug           emit `#line` directives mapping the generated C back\n"
        "                    to `.tur` source, and compile single-file builds with\n"
        "                    `-g -O0` so gdb/lldb step through Turmeric source.\n"
        "  --timings[=<p>]   print where the compile's time went, per phase\n"
        "                    (exclusive time; phases sum to the wall clock), and\n"
        "                    with <p> write it as tur-profile/1 JSON.  Also on\n"
        "                    check, emit-c, run and jit.  Profile a Release tur.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_run(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur run [-I <dir>...] <file.tur> [-- <args...>]\n"
        "                                  build and execute a single file\n"
        "  tur run [-I <dir>...] - [-- <args...>]\n"
        "                                  read source from stdin, build and execute\n"
        "\n"
        "flags:\n"
        "  -I <dir>    add an include directory for module resolution\n"
        "  --release   compile at -O3 (the default is -O2)\n"
        "  --offline   skip dependency fetch\n"
        "  --          pass remaining arguments to the program\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_expand(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur expand [-I <dir>...] [--no-auto-spice] [--no-auto-stdlib] <file.tur>\n"
        "                                       elaborate the file and print each macro\n"
        "                                       expansion to stdout as it happens\n"
        "\n"
        "Each expansion prints as a `;; <macro-name> @ <line>:<col>` header (the\n"
        "call site) followed by the expanded form.  Nested expansions print too,\n"
        "inner-first, in elaboration order.  Diagnostics still go to stderr, so\n"
        "stdout is suitable for golden-file comparison.\n"
        "\n"
        "flags: same as `tur check` (see `tur check --help`)\n");
    return 0;
}

int usage_check(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur check [-I <dir>...] [--no-auto-spice] [--no-auto-stdlib] <file.tur>\n"
        "                                       type-check only, no codegen\n"
        "\n"
        "flags:\n"
        "  -I <dir>             add an include directory for module resolution\n"
        "                       (repeat to add multiple)\n"
        "  --no-auto-spice      don't auto-discover the enclosing spice's src/\n"
        "                       (default behavior walks up from the file looking\n"
        "                       for build.tur and adds <spice>/src to the path;\n"
        "                       with no build.tur, the installed spices instead)\n"
        "  --no-auto-stdlib     when the checked file is one of the auto-loaded\n"
        "                       stdlib files, skip auto-loading it and all\n"
        "                       subsequent stdlib files; earlier stdlib files\n"
        "                       still load so the file's own deps resolve.\n"
        "                       Use when type-checking auto-loaded stdlib files\n"
        "                       in isolation to avoid duplicate-definition errors.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

/* SC1: is argv[i] the start of a `-I <path>` or `-I<path>` flag?
 * On true, writes the number of argv positions it consumes into *consumed
 * (1 for "-Ifoo", 2 for "-I foo").  Callers loop over argv looking for
 * non-include flags and skip these positions. */
bool drv_is_include_flag(int argc, char **argv, int i, int *consumed) {
    const char *a = argv[i];
    if (strcmp(a, "-I") == 0 && i + 1 < argc) {
        *consumed = 2;
        return true;
    }
    if (strncmp(a, "-I", 2) == 0 && a[2] != '\0') {
        *consumed = 1;
        return true;
    }
    return false;
}

/* SC1: collect every -I flag from argv[start..argc-1] into a heap-allocated
 * array.  The array of pointers is malloc'd (caller frees); the strings it
 * points to are borrowed from argv and live as long as argv does.
 *
 * Returns the number of include dirs collected, or -1 if a bare `-I` was
 * encountered without a following path argument.  Non-`-I` arguments are
 * ignored -- callers handle them in their own pass (using drv_is_include_flag
 * to skip the positions consumed here). */
int drv_parse_include_flags(int argc, char **argv, int start, char ***out_dirs) {
    int  cap = 4;
    int  n   = 0;
    char **dirs = (char **)malloc((size_t)cap * sizeof(char *));
    if (!dirs) { *out_dirs = NULL; return 0; }
    for (int i = start; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-I") == 0) {
            if (i + 1 >= argc) { free(dirs); *out_dirs = NULL; return -1; }
            if (n >= cap) {
                cap *= 2;
                char **bigger = (char **)realloc(dirs, (size_t)cap * sizeof(char *));
                if (!bigger) { free(dirs); *out_dirs = NULL; return n; }
                dirs = bigger;
            }
            dirs[n++] = argv[++i];
        } else if (strncmp(a, "-I", 2) == 0 && a[2] != '\0') {
            if (n >= cap) {
                cap *= 2;
                char **bigger = (char **)realloc(dirs, (size_t)cap * sizeof(char *));
                if (!bigger) { free(dirs); *out_dirs = NULL; return n; }
                dirs = bigger;
            }
            dirs[n++] = argv[i] + 2;
        }
    }
    *out_dirs = dirs;
    return n;
}

int usage_eval(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur eval '<expr>'          evaluate an inline expression and print the result\n"
        "  tur eval --file <file.tur> run a .tur file through the interpreter\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_doc(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur doc <symbol>          print documentation for a builtin, special\n"
        "                            form, or stdlib definition\n"
        "  tur doc --json <symbol>   print documentation as JSON\n"
        "\n"
        "Reads the docstring table shipped alongside the binary\n"
        "(<stdlib>/docstrings.tur); no network access is involved.\n"
        "For rendered guides and API pages offline, see `tur docs`.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_format(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur format [file.tur]          format a source file (stdin if no file given)\n"
        "  tur format --check [file.tur]  exit 1 if formatting would change the file\n"
        "  tur format --diff [file.tur]   print unified diff of formatting changes\n"
        "  tur format --lang <dialect>    read with that dialect (as `tur fmt --lang`);\n"
        "                                 default: the file's extension and #lang line\n"
        "\n"
        "  Prefer `tur fmt`, which also formats in place.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_parse_check(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur parse-check <a> <b>   exit 0 if both files read to the same AST\n"
        "\n"
        "  <a> is read as turmeric, <b> as sweet-exp, unless an explicit\n"
        "  #lang directive overrides. Exit codes: 0 equal, 1 mismatch, 2 error.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_test(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur test <dir>   run all .tur test files in a directory\n"
        "\n"
        "Each test compiles and runs; it passes iff both succeed (exit 0).\n"
        "A test file may carry directives in its leading comment lines:\n"
        "  ;; tur-test-flags: --strict-refine   extra compile flags for this test\n"
        "  ;; tur-test-expect-error: TUR-W0372  this test must FAIL to compile and\n"
        "                                       its diagnostics must contain the\n"
        "                                       text; the run phase is skipped\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_repl(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur repl [-I <dir>...] [--watch] [--lang <dialect>] [--engine <name>]\n"
        "                  start the interactive REPL\n"
        "\n"
        "flags:\n"
        "  -I <dir>        add a module search directory (repeatable); searched\n"
        "                  before the enclosing spice or, outside a project,\n"
        "                  before the installed spices\n"
        "  --lang <d>      start in a language dialect: \"turmeric\" (default),\n"
        "                  \"saffron\" (unannotated params default to any),\n"
        "                  \"r7rs\" (Scheme), or any base `tur dialects` lists.\n"
        "                  Equivalent to typing `#lang <d>` at the prompt.\n"
        "  --watch         auto-reload the enclosing spice between prompts\n"
        "                  when any source .tur file's mtime advances\n"
        "                  (RP6; equivalent to typing (reload) each turn)\n"
        "  --engine <name> engine for building the enclosing spice:\n"
        "                  \"cc\" (default -- build --shared subprocess) or\n"
        "                  \"jit\" (in-process MIR, needs -DTUR_JIT=ON).\n"
        "                  Precedence: --engine > TUR_ENGINE > build.tur\n"
        "                  :engine > \"cc\"\n"
        "\n"
        "REPL commands:\n"
        "  :help           print help\n"
        "  :quit / :q      exit the REPL\n"
        "  :doc <sym>      look up documentation for a symbol\n"
        "  :type <expr>    print the inferred type of an expression\n"
        "  :reload <file>  reload a source file\n"
        "  :reset          clear session and start fresh\n"
        "  :spice [name]   load an installed spice (or a checkout, by path);\n"
        "                  bare :spice lists loaded and installed spices\n"
        "  :tutorial       start the interactive tutorial\n"
        "\n"
        "REPL forms:\n"
        "  (reload)        rebuild the loaded spice (and any attached spice\n"
        "                  whose sources changed) and refresh bindings\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

int usage_explain(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur explain <TUR-E####>    print explanation for a diagnostic code\n"
        "  tur explain '<snippet>'   compile a snippet and explain errors\n"
        "\n"
        "examples:\n"
        "  tur explain TUR-E0042\n"
        "  tur explain '(+ 1 \"x\")'\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

/* SC4: --no-auto-spice disables the per-file subcommand walk-up that
 * adds the enclosing spice's `src/` to the include path.  Inspected by
 * auto_append_spice_includes(). */
bool g_no_auto_spice = false;

/* --no-auto-stdlib (tur check only): suffix-skip -- when the file being
 * checked IS one of the auto-loaded stdlib files, skip auto-loading it
 * and all subsequent stdlib files.  Earlier entries still load so the
 * file's transitive dependencies resolve.  This prevents duplicate-
 * definition errors when type-checking auto-loaded stdlib files in isolation. */
bool g_no_auto_stdlib = false;

/* J6: --no-abi-cache (or TUR_NO_ABI_CACHE=1) disables the persistent
 * cross-module ABI specialization cache under <build-root>/.tur-abi-cache/.
 * The cache is a build-time optimization; disabling it must always still
 * produce a correct build (clones are recomputed per invocation). */
bool g_no_abi_cache = false;

/* ===========================================================================
 * Startup: the environment's switches and --timings
 * ======================================================================== */

/* The switches the environment can flip, read once at startup.  None of them
 * is a CLI flag, on purpose: each is a bisection hatch for something that
 * graduated to the default, or (TUR_RUNTIME) the default for a flag a
 * subcommand may still override. */
void drv_apply_env_overrides(void) {
    /* TUR_ADT_SLAB=1: bump-allocate never-freed multi-variant ADT boxes.
     * Env-only measurement seam (see
     * docs/archive/multi-variant-adts-always-heap-allocate.md), deliberately
     * not a CLI flag -- and SHELVED 2026-08-25, so it is not a shipping
     * feature and is not on its way to becoming one. */
    {
        const char *__slab = getenv("TUR_ADT_SLAB");
        if (__slab && __slab[0] == '1') g_adt_slab = true;
        /* SR1 is ON by default; the env var is a two-way override so a
         * suspected representation bug can be bisected against the old int64
         * carrier without rebuilding.  `=0` opts out, `=1` is a no-op that
         * keeps existing invocations working. */
        const char *__sr1 = getenv("TUR_SR1_SUM_BYVALUE");
        if (__sr1 && __sr1[0] == '0') g_sr1_sum_byvalue = false;
        else if (__sr1 && __sr1[0] == '1') g_sr1_sum_byvalue = true;
        /* SR2a is ON by default too, since its graduation out of
         * --enable=parametric-sum-byvalue.  Same two-way shape: `=0` restores
         * the int64 carrier for a parametric sum monomorph, `=1` is the no-op
         * that keeps the measurement harnesses' existing invocations working.
         *
         * `=0` also takes SR3's Option niche down with it, in sr3_option_niche
         * (types.c) rather than here -- the niche narrows a BY-VALUE Option to
         * its payload pointer, so it cannot outlive the representation it
         * narrows.  Setting this alone is therefore a complete switch; it did
         * not used to be, and the failure mode was a compiler abort two layers
         * from the flag (docs/archive/sr2-carrier-seam-rotted.md). */
        const char *__sr2 = getenv("TUR_SR2_APP_SUM_BYVALUE");
        if (__sr2 && __sr2[0] == '0') g_sr2_app_sum_byvalue = false;
        else if (__sr2 && __sr2[0] == '1') g_sr2_app_sum_byvalue = true;
        /* SR3 slice B (the Option niche) is ON by default since its graduation
         * out of --enable=option-niche (2026-09-03).  `=0` restores the tagged
         * 16-byte Option monomorph and the boxed Vec slot for bisection and
         * for tests/run-option-niche-seam.sh, which keeps that path green.
         *
         * One-directional: this turns the niche off without disturbing SR2,
         * whereas TUR_SR2_APP_SUM_BYVALUE=0 turns BOTH off (the niche needs a
         * by-value Option to narrow).  So `=1` here cannot force the niche back
         * on under a disabled SR2 -- sr3_option_niche refuses it, deliberately,
         * because that combination is the one that aborts the compiler. */
        const char *__sr3 = getenv("TUR_OPTION_NICHE");
        if (__sr3 && __sr3[0] == '0') g_opt_option_niche = false;
        else if (__sr3 && __sr3[0] == '1') g_opt_option_niche = true;
        /* RM3 regions are ON by default since graduation out of
         * --enable=regions (2026-09-05).  `=0` restores plain malloc at the
         * routed ctor sites, no bracket around bt-scope / with-region, and no
         * atexit shutdown -- the pre-graduation default build -- for bisection
         * and for tests/run-regions-seam.sh, which keeps that path green.
         * Independent of the SR2/SR3 hatches above: a region is a lifetime
         * over the arena, not a representation, so it composes with either. */
        const char *__rgn = getenv("TUR_REGIONS");
        if (__rgn && __rgn[0] == '0') g_opt_regions = false;
        else if (__rgn && __rgn[0] == '1') g_opt_regions = true;
        /* The interpreter's inline-C JIT is ON by default since graduation
         * out of --enable=repl-jit-inline-c (2026-10-17).  `=0` restores the
         * "inline-C not supported" error -- the pre-graduation default -- for
         * bisection and for the harnesses that test the pattern executor on
         * its own (run-interp-*-inline-c*.sh); tests/turi/repl-jit-inline-c.sh
         * keeps that path covered. */
        const char *__ijit = getenv("TUR_REPL_JIT_INLINE_C");
        if (__ijit && __ijit[0] == '0') g_opt_repl_jit_inline_c = false;
        else if (__ijit && __ijit[0] == '1') g_opt_repl_jit_inline_c = true;
    }
    /* tur-link-and-build-split-plan Phase 2/3c/6: TUR_RUNTIME overrides the
     * default runtime-linkage mode (auto).  A CLI --runtime= flag, parsed
     * later, still wins. */
    {
        const char *rt = getenv("TUR_RUNTIME");
        if (rt) {
            if      (strcmp(rt, "lib") == 0)    g_runtime_mode = TUR_RT_LIB;
            else if (strcmp(rt, "source") == 0) g_runtime_mode = TUR_RT_SOURCE;
            else if (strcmp(rt, "auto") == 0)   g_runtime_mode = TUR_RT_AUTO;
            else if (strcmp(rt, "split") == 0)  g_runtime_mode = TUR_RT_SPLIT;
        }
    }
}

/* profiling-plan C0+C1: does `--timings <arg>` mean <arg> is the JSON
 * destination?  The two-token form cannot otherwise be told apart from
 * `--timings <input-file>`, so it takes the next argument only when that is
 * unmistakably an output path: one ending in `.json`, or a /dev/ node
 * (/dev/stdout, /dev/stderr, /dev/fd/N).  `--timings=<path>` takes any path,
 * and `--timings=-` means stdout. */
static bool timings_path_like(const char *a) {
    if (!a || !*a) return false;
    if (strncmp(a, "/dev/", 5) == 0) return true;
    size_t n = strlen(a);
    return n > 5 && strcmp(a + n - 5, ".json") == 0;
}

/* --timings / --timings=<path> / --timings <path>: start the zone timer and
 * strip the flag (and its path) from argv.  On any command -- the zones are
 * where the compiler is, so a command that compiles nothing reports "other".
 * Stops at `--`: what follows is the program's own argv. */
void drv_parse_timings_flag(int *argc_io, char **argv) {
    int argc = *argc_io;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) break;
        int take = 0;
        const char *path = NULL;
        if (strcmp(argv[i], "--timings") == 0) {
            take = 1;
            if (i + 1 < argc && timings_path_like(argv[i + 1])) {
                path = argv[i + 1];
                take = 2;
            }
        } else if (strncmp(argv[i], "--timings=", 10) == 0) {
            take = 1;
            path = argv[i] + 10;
        }
        if (take == 0) continue;
        prof_start(argc, argv, path);   /* the command as typed, flag and all */
        for (int j = i; j + take < argc; j++) argv[j] = argv[j + take];
        argc -= take;
        i--;
    }
    *argc_io = argc;
}

/* ===========================================================================
 * The global flags
 *
 * A global flag may appear anywhere before `--`, before or after the
 * subcommand, and every command sees argv with it removed.  One table lists
 * them all.  drv_parse_global_flags reads it in two passes, the way the code
 * it replaced did, and the difference is observable:
 *
 *   1. The SCAN pass applies every GF_SCAN flag wherever it appears, before
 *      anything is removed, so `--debug` counts even as the word after
 *      `--explain`.  The environment's half of a flag (TUR_NO_ABI_CACHE,
 *      TUR_R7RS_GC, TUR_SAFFRON_GC) is read first, so the flag wins.
 *   2. The STRIP pass walks argv left to right, applies each flag that is not
 *      a scan flag, and removes every flag the table knows except a GF_KEEP
 *      one.  A flag whose action fails stops the parse with its status.
 *
 * A word is matched against the table in table order.  A GF_VALUE flag with
 * no word after it is not that flag at all: it stays in argv, and the
 * subcommand rejects it.  To add a flag, see the build-driver guide (section
 * "A new global flag").
 * ======================================================================== */

bool        g_no_color         = false;
bool        g_json_diagnostics = false;
const char *g_explain_code     = NULL;

enum {
    GF_SCAN   = 1u << 0,  /* applied by the scan pass, wherever it appears */
    GF_KEEP   = 1u << 1,  /* read, but left in argv: the commands skip it */
    GF_VALUE  = 1u << 2,  /* takes the next word as its value */
    GF_PREFIX = 1u << 3,  /* the spelling ends in '=' and the value follows */
};

typedef struct GlobalFlag {
    const char *spelling;
    const char *alt;                 /* a second spelling, or NULL */
    unsigned    how;                 /* GF_* */
    bool       *set;                 /* set to `to` when applied, or NULL */
    bool        to;
    /* Run after `set`, with the value (GF_VALUE, GF_PREFIX) or the word
     * itself.  Nonzero stops the parse; the dispatcher exits with it. */
    int       (*apply)(const char *arg);
    /* Matches the word instead of `spelling` (the -X family), or NULL. */
    bool      (*match)(const char *word);
} GlobalFlag;

static int gf_panic_trace(const char *arg) {
    (void)arg;
    /* Retired (panic-location-names-the-runtime-not-the-call-site): it
     * printed the defer-frame chain as raw pointers, which named no function
     * and no line.  A panic now names its own .tur file and line, and
     * `--debug` builds for a debugger's backtrace.  Still accepted so
     * existing scripts keep working; TUR-W0050 says so. */
    fprintf(stderr, "warning [TUR-W0050]: --panic-trace is retired and "
                    "does nothing; a panic already names its .tur file "
                    "and line, and a --debug build gives a debugger "
                    "backtrace (`tur debug`)\n");
    return 0;
}

static int gf_strict_effects(const char *arg) {
    (void)arg;
    g_strict_effects = true;
    return 0;
}

static int gf_lint_effects(const char *arg) {
    (void)arg;
    /* Retired: it was a byte-identical copy of --strict-effects
     * (docs/archive/strict-effects-and-lint-effects-are-
     * indistinguishable.md).  Still accepted, as an alias, so existing
     * scripts keep working; TUR-W0050 says so. */
    fprintf(stderr, "warning [TUR-W0050]: --lint-effects is deprecated; "
                    "use --strict-effects, which emits the same "
                    "TUR-W0030 warnings (-Werror=strict-effects makes "
                    "them errors)\n");
    g_strict_effects = true;
    return 0;
}

static int gf_explain(const char *arg) {
    g_explain_code = arg;
    return 0;
}

static int gf_lint_unsafe(const char *arg) {
    (void)arg;
    g_lint_unsafe_enabled = true;
    g_unsafe_warn_nested = true;
    return 0;
}

static int gf_lint_unsafe_max_lines(const char *arg) {
    g_lint_unsafe_enabled = true;
    g_unsafe_max_lines = (uint32_t)atoi(arg);
    return 0;
}

static int gf_require_unsafe_docs(const char *arg) {
    (void)arg;
    g_lint_unsafe_enabled = true;
    g_unsafe_require_safety = true;
    return 0;
}

static int gf_unsafe_stats(const char *arg) {
    (void)arg;
    g_lint_unsafe_enabled = true;
    g_unsafe_stats_enabled = true;
    return 0;
}

/* Phase B5: cap run-backtrack at N results (0 = unlimited). */
static int gf_backtrack_depth(const char *arg) {
    g_backtrack_depth = (int64_t)atoll(arg);
    return 0;
}

/* drop-x-flags-plan: every -X<name> is an accept-and-warn no-op as of
 * v0.24.0.  TUR-W0050 emitted; the underlying feature is always on (see
 * src/runtime/globals.c). */
static int gf_x_flag(const char *word) {
    warn_deprecated_x_flag(word);
    return 0;
}

/* XF1: opt in to one or more experimental features (comma list).  An
 * unknown name is a hard TUR-E0310 error. */
static int gf_enable(const char *list) {
    return enable_experiment_list(list, XF_SRC_CLI) ? 0 : 2;
}

/* Stage 3 (macro-system-direction-plan): grant the macro-time env extra
 * capabilities.  Only `io` exists; anything else -- including a plea for
 * ffi/unsafe -- is a hard error, because those are never offered at macro
 * time. */
static int gf_macro_caps(const char *cap) {
    if (strcmp(cap, "io") != 0) {
        fprintf(stderr,
                "error: --macro-caps only accepts 'io' "
                "(got '%s'); ffi/unsafe/inline-c/async are never "
                "available at macro-expansion time\n",
                cap);
        return 2;
    }
    g_macro_caps_io = true;
    return 0;
}

/* UC-4 (user-config-experiments-plan): --allow-experimental was retired.
 * Enabling an experiment (via --enable=<name>, build.tur, or
 * ~/.config/turmeric/experiments.tur) is now itself the acknowledgment; the
 * separate gate added nothing.  Reject it with a targeted message rather than
 * silently accepting it or emitting a generic "unknown flag". */
static int gf_allow_experimental(const char *arg) {
    (void)arg;
    fprintf(stderr,
            "error: --allow-experimental was retired in v" TUR_VERSION
            "; enabling an experiment (via --enable=<name>, build.tur, "
            "or ~/.config/turmeric/experiments.tur) is now the "
            "acknowledgment. Remove the flag.\n");
    return 2;
}

static const GlobalFlag GLOBAL_FLAGS[] = {
    /* Phase R5: all panics call abort() directly. */
    { "--panic-abort", NULL, GF_SCAN, &g_panic_abort, true, NULL, NULL },
    /* SC4: no enclosing-spice auto-discovery in the per-file subcommands
     * (check/emit-c/emit-h/run). */
    { "--no-auto-spice", NULL, GF_SCAN, &g_no_auto_spice, true, NULL, NULL },
    /* installed-spice-libraries D8: keep the installed spices off the search
     * path of a file outside any project.  --no-auto-spice implies it (it
     * skips the whole walk); TUR_NO_GLOBAL_SPICES=1 is the env spelling. */
    { "--no-global-spices", NULL, GF_SCAN, &g_no_global_spices, true, NULL, NULL },
    /* r7rs-gc: build a Scheme (or Saffron) program without the collector --
     * a program that starts threads.  TUR_R7RS_GC=0 / TUR_SAFFRON_GC=0 too. */
    { "--no-r7rs-gc", NULL, GF_SCAN, &g_opt_r7rs_gc, false, NULL, NULL },
    { "--no-saffron-gc", NULL, GF_SCAN, &g_opt_saffron_gc, false, NULL, NULL },
    { "--panic-trace", NULL, 0, NULL, false, gf_panic_trace, NULL },
    /* Phase R6: the last of the two wins. */
    { "--warn-unused-result", NULL, GF_SCAN, &g_warn_unused_result, true, NULL, NULL },
    { "--no-warn-unused-result", NULL, GF_SCAN, &g_warn_unused_result, false, NULL, NULL },
    /* F4: what it promotes is listed at g_werror_deprecated (globals.c). */
    { "--Werror=deprecated", "-Werror=deprecated", GF_SCAN,
      &g_werror_deprecated, true, NULL, NULL },
    /* Phase C: narrow-param-in-inline-C warnings become errors. */
    { "--Werror=inline-c-narrow-params", "-Werror=inline-c-narrow-params", GF_SCAN,
      &g_werror_inline_c_narrow_params, true, NULL, NULL },
    /* The --strict-effects lints become errors; it implies --strict-effects. */
    { "--Werror=strict-effects", "-Werror=strict-effects", GF_SCAN,
      &g_werror_strict_effects, true, gf_strict_effects, NULL },
    /* Phase R6: lint panic/must! usage. */
    { "--lint-panic", NULL, GF_SCAN, &g_lint_panic, true, NULL, NULL },
    /* Phase C2: strip contract checks (release builds). */
    { "--no-contracts", NULL, GF_SCAN, &g_no_contracts, true, NULL, NULL },
    /* Debugger Phase 4: `#line` directives, and -g -O0 builds.  Stripped so
     * the per-command flag parsers (build/emit-c) do not reject it. */
    { "--debug", NULL, GF_SCAN, &g_emit_debug_lines, true, NULL, NULL },
    { "--json-diagnostics", NULL, 0, &g_json_diagnostics, true, NULL, NULL },
    /* E14: structured JSON output; implies --json-diagnostics. */
    { "--json", NULL, 0, &use_json_output, true, NULL, NULL },
    { "--explain", NULL, GF_VALUE, NULL, false, gf_explain, NULL },
    { "--lint-unsafe", NULL, 0, NULL, false, gf_lint_unsafe, NULL },
    { "--lint-unsafe-max-lines=", NULL, GF_PREFIX, NULL, false, gf_lint_unsafe_max_lines, NULL },
    { "--require-unsafe-docs", NULL, 0, NULL, false, gf_require_unsafe_docs, NULL },
    { "--unsafe-stats", NULL, 0, NULL, false, gf_unsafe_stats, NULL },
    { "--lint-inline-c-unsafe", NULL, 0, &g_lint_inline_c_unsafe, true, NULL, NULL },
    /* Phase HKT-P6: dump kind annotations after kind-check. */
    { "--dump-kinds", NULL, 0, &g_dump_kinds, true, NULL, NULL },
    /* RT3: every undecided or refuted refinement obligation becomes a hard
     * compile error rather than a runtime check.  A diagnostic-strictness
     * knob, NOT an experiment -- it changes how loudly the compiler reports,
     * not what it compiles. */
    { "--strict-refine", NULL, 0, &g_strict_refine, true, NULL, NULL },
    /* ER1: unannotated effectful functions are warnings. */
    { "--strict-effects", NULL, 0, &g_strict_effects, true, NULL, NULL },
    /* reflected-measures RF5: the totality verdict for each `^reflect` site. */
    { "--dump-reflect", NULL, 0, &g_dump_reflect, true, NULL, NULL },
    /* G1: the checked verdict for each declared `#writes` frame. */
    { "--dump-write-frames", NULL, 0, &g_dump_write_frames, true, NULL, NULL },
    /* R4 slice 2: the verification verdict for each `#reads` frame. */
    { "--dump-read-frames", NULL, 0, &g_dump_read_frames, true, NULL, NULL },
    /* ER6: the inferred effect row for each defn. */
    { "--dump-effects", NULL, 0, &g_dump_effects, true, NULL, NULL },
    /* SX8a: one JSON record per refinement obligation, on stdout.  Global
     * rather than per-command so it works with `check` and `emit-c` alike --
     * the records come from the elaboration both share. */
    { "--dump-refine=json", NULL, 0, &g_dump_refine_json, true, NULL, NULL },
    /* CPS1: the whole-program may-capture coloring per defn. */
    { "--dump-cps-coloring", NULL, 0, &g_dump_cps_coloring, true, NULL, NULL },
    /* CPS2: the CT-IR for each colored defn. */
    { "--dump-cps", NULL, 0, &g_dump_cps, true, NULL, NULL },
    /* VBM1: the by-value HKT monomorphization spec registry. */
    { "--dump-mono-specs", NULL, 0, &g_dump_mono_specs, true, NULL, NULL },
    /* G1 (cps-backend-generic-monomorph-classification-plan): CPS-subset
     * admissibility of each colored-generic monomorph. */
    { "--dump-cps-mono", NULL, 0, &g_dump_cps_mono, true, NULL, NULL },
    /* SZ8: the inferred size index per sized-GADT constructor. */
    { "--dump-sizes", NULL, 0, &g_dump_sizes, true, NULL, NULL },
    { "--lint-effects", NULL, 0, NULL, false, gf_lint_effects, NULL },
    { "--backtrack-depth=", NULL, GF_PREFIX, NULL, false, gf_backtrack_depth, NULL },
    { "--backtrack-depth", NULL, GF_VALUE, NULL, false, gf_backtrack_depth, NULL },
    /* Phase B5: the cloneable capture plan after CPS. */
    { "--dump-clone-plan", NULL, 0, &g_dump_clone_plan, true, NULL, NULL },
    /* CPS3: emit CPS wrappers for colored functions. */
    { "--cps-path", NULL, 0, &g_cps_path, true, NULL, NULL },
    /* Phase I: trace the resolved ABI path for each call site during emit. */
    { "--emit-abi-trace", NULL, 0, &g_emit_abi_trace, true, NULL, NULL },
    { "-X", NULL, 0, NULL, false, gf_x_flag, is_known_deprecated_x_flag },
    /* CT3: keep contract checks in release builds. */
    { "--keep-contracts", NULL, 0, &g_keep_contracts_in_release, true, NULL, NULL },
    { "--enable=", NULL, GF_PREFIX, NULL, false, gf_enable, NULL },
    { "--macro-caps=", NULL, GF_PREFIX, NULL, false, gf_macro_caps, NULL },
    /* security-audit-plan WP3: no defmacro* body runs. */
    { "--no-proc-macros", NULL, 0, &g_no_proc_macros, true, NULL, NULL },
    { "--allow-experimental", NULL, 0, NULL, false, gf_allow_experimental, NULL },
    /* no-color-flag-rejected-everywhere: it was once read but never
     * stripped, so a trailing one reached the command's parser as an unknown
     * flag and a leading one was taken for the command name. */
    { "--no-color", NULL, GF_SCAN, &g_no_color, true, NULL, NULL },
    /* J6: no persistent cross-module ABI cache (.tur-abi-cache/).  Read but
     * not stripped; the commands that take it skip it themselves.
     * TUR_NO_ABI_CACHE=1 too. */
    { "--no-abi-cache", NULL, GF_SCAN | GF_KEEP, &g_no_abi_cache, true, NULL, NULL },
};

static const GlobalFlag *gf_lookup(char **argv, int argc, int i) {
    const char *w = argv[i];
    for (size_t k = 0; k < sizeof GLOBAL_FLAGS / sizeof GLOBAL_FLAGS[0]; k++) {
        const GlobalFlag *f = &GLOBAL_FLAGS[k];
        bool hit;
        if (f->match)
            hit = f->match(w);
        else if (f->how & GF_PREFIX)
            hit = strncmp(w, f->spelling, strlen(f->spelling)) == 0;
        else
            hit = strcmp(w, f->spelling) == 0 || (f->alt && strcmp(w, f->alt) == 0);
        if (hit && (f->how & GF_VALUE) && i + 1 >= argc) hit = false;
        if (hit) return f;
    }
    return NULL;
}

/* The value a flag's action receives: the word after it, the text after its
 * '=', or the word itself. */
static const char *gf_arg(const GlobalFlag *f, char **argv, int i) {
    if (f->how & GF_VALUE)  return argv[i + 1];
    if (f->how & GF_PREFIX) return argv[i] + strlen(f->spelling);
    return argv[i];
}

int drv_parse_global_flags(int *argc_io, char **argv) {
    int argc = *argc_io;

    /* global-flags-parsed-after-double-dash: everything from the first `--`
     * on belongs to the program, so the global flags are parsed and stripped
     * from the words before it only.  `argc` is cut there for the parse, and
     * the tail (the `--` included, which the commands that forward arguments
     * look for) is put back after the strip pass.  The strip only moves
     * words below the cut, so the tail is never touched in between. */
    int dd_at = argc;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--") == 0) { dd_at = i; break; }
    const int dd_tail = argc - dd_at;
    argc = dd_at;

    /* The environment's half of three scan flags; the flag, read next,
     * can only turn the same thing on. */
    const char *e = getenv("TUR_NO_ABI_CACHE");
    if (e && *e && strcmp(e, "0") != 0) g_no_abi_cache = true;
    e = getenv("TUR_R7RS_GC");
    if (e && e[0] == '0') g_opt_r7rs_gc = false;
    e = getenv("TUR_SAFFRON_GC");
    if (e && e[0] == '0') g_opt_saffron_gc = false;

    /* Pass 1: scan. */
    for (int i = 1; i < argc; i++) {
        for (size_t k = 0; k < sizeof GLOBAL_FLAGS / sizeof GLOBAL_FLAGS[0]; k++) {
            const GlobalFlag *f = &GLOBAL_FLAGS[k];
            if (!(f->how & GF_SCAN)) continue;
            if (strcmp(argv[i], f->spelling) != 0 &&
                !(f->alt && strcmp(argv[i], f->alt) == 0)) continue;
            if (f->set) *f->set = f->to;
            if (f->apply) (void)f->apply(argv[i]);
        }
    }

    /* Pass 2: apply the rest, and strip. */
    for (int i = 1; i < argc; i++) {
        const GlobalFlag *f = gf_lookup(argv, argc, i);
        if (!f || (f->how & GF_KEEP)) continue;
        if (!(f->how & GF_SCAN)) {
            if (f->set) *f->set = f->to;
            if (f->apply) {
                int rc = f->apply(gf_arg(f, argv, i));
                if (rc) return rc;
            }
        }
        int take = (f->how & GF_VALUE) ? 2 : 1;
        for (int j = i; j < argc - take; j++) argv[j] = argv[j + take];
        argc -= take;
        i--;
    }

    /* Put the program's words back (see dd_at above). */
    if (dd_tail > 0 && argc != dd_at)
        memmove(&argv[argc], &argv[dd_at], (size_t)dd_tail * sizeof(argv[0]));
    argc += dd_tail;
    argv[argc] = NULL;
    *argc_io = argc;
    return 0;
}

/* ===========================================================================
 * The commands
 *
 * One table drives the dispatcher (drv_find_command), prefix resolution,
 * the `tur --help` listing (drv_usage), the filter that keeps a shadowed
 * `tur-<name>` on $PATH out of the "external commands" listing, and the
 * command lists in the completion scripts.  Before it there were three lists,
 * and they disagreed.
 *
 * Its order is the order `tur --help` lists the commands in.  A DRV_CMD_EXACT
 * command dispatches on its exact spelling only and takes no part in prefix
 * resolution: `completion` would make `tur comp` (today `compile`)
 * ambiguous, and `jit` would do the same to `tur j` (the `just` alias).
 * `emit-rt-split` is the generator tools/gen-runtime-split.py runs, not a
 * command anyone types.  To add a command, see the build-driver guide
 * (section "A new subcommand").
 * ======================================================================== */

static int run_dap(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Debugger Phase 3: DAP server over the interpreter (JSON-RPC / stdio). */
    return cmd_dap();
}

static int run_worker(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Tier 3: persistent fixture worker for the test suite. */
    return cmd_worker();
}

static int run_completion(int argc, char **argv);

static const DrvCommand COMMANDS[] = {
    { "build", cmd_build_main, 0,
      "build a file or directory into an executable",
      "  tur build <file.tur> [-o <out>]    build a single file\n"
      "  tur build <dir> [-o <out>]         build all .tur files in directory\n" },
    { "compile", cmd_compile_main, 0,
      "lower a .tur to an object file",
      "  tur compile <file.tur> -o <out.o>  lower a .tur to an object (+ .link sidecar)\n" },
    { "link", cmd_link_main, 0,
      "link objects into an executable",
      "  tur link <obj/src>... -o <out>     link objects (+ .link sidecars) into an exe\n" },
    { "emit-c", cmd_emit_c_main, 0,
      "print the generated C to stdout",
      "  tur emit-c <input.tur>            print the generated C to stdout\n" },
    { "emit-h", cmd_emit_h_main, 0,
      "print the generated header to stdout",
      "  tur emit-h <input.tur>            print the generated header to stdout\n" },
    { "run", cmd_run_main, 0,
      "run a Justfile recipe, or build and execute a .tur file",
      "  tur run <input.tur>               build + execute a single file\n"
      "  tur run <recipe>                  run a Justfile recipe (`tur just` is a synonym)\n" },
    { "repl", cmd_repl_main, 0,
      "interactive REPL",
      "  tur repl                          interactive REPL (Phase S1)\n" },
    { "worker", run_worker, 0,
      "persistent fixture evaluator",
      "  tur worker                        persistent fixture evaluator (Tier 3, reads dirs from stdin)\n" },
    { "interpret", cmd_interpret_main, 0,
      "run a file through the tree-walking interpreter",
      "  tur interpret [-I dir] <file.tur> run a file through the tree-walking interpreter\n" },
    { "debug", cmd_debug_main, 0,
      "run a file under the interactive debugger",
      "  tur debug [-I dir] <file.tur>     run a file under the interactive debugger\n" },
    { "dap", run_dap, 0,
      "Debug Adapter Protocol server",
      "  tur dap                           Debug Adapter Protocol server (JSON-RPC/stdio) for editors\n" },
    { "trace", cmd_trace_main, 0,
      "record an interpreted run, or read one back",
      "  tur trace <file.tur> [-o f]       record an interpreted run; --dump reads one back\n" },
    { "lsp-lite", cmd_lsp_lite_main, 0,
      "lightweight completion/calltip/doc backend",
      "  tur lsp-lite                      lightweight completion/calltip/doc backend (NDJSON/stdio)\n" },
    { "eval", cmd_eval_main, 0,
      "evaluate an inline expression",
      "  tur eval '<expr>'                 evaluate an inline expression\n" },
    { "doc", cmd_doc_main, 0,
      "print documentation for a symbol",
      "  tur doc <symbol>                  print documentation for a builtin, special form, or stdlib definition\n" },
    { "docs", cmd_docs, 0,
      "locate, open, or serve the rendered guides",
      "  tur docs [--open|--serve]         locate, open, or serve the rendered guides and API reference\n" },
    { "explain", cmd_explain_main, 0,
      "explain a diagnostic code or snippet",
      "  tur explain <TUR-E####|snippet>   explain a diagnostic code or snippet errors\n" },
    { "test", cmd_test_main, 0,
      "run all .tur files in a directory",
      "  tur test <dir>                    run all .tur files in a directory\n" },
    { "check", cmd_check_main, 0,
      "type-check only, no codegen",
      "  tur check <input.tur>             type-check only, no codegen (phase 8)\n"
      "  tur check --dump-refine=json <f>  print one JSON record per refinement obligation\n" },
    { "expand", cmd_expand_main, 0,
      "trace macro expansions while checking",
      "  tur expand <input.tur>            trace macro expansions to stdout while checking\n" },
    { "format", cmd_format_main, 0,
      "format a single source file or stdin",
      "  tur format [--check|--diff] [file.tur]   format source (stdin if no file given)\n" },
    { "fmt", cmd_fmt, 0,
      "format sources in place, walking directories",
      "  tur fmt [--check|--diff|--dry-run] [paths...]  format in place with dir walking\n" },
    { "parse-check", cmd_parse_check_main, 0,
      "exit 0 if two files read to the same AST",
      "  tur parse-check <a> <b>           exit 0 if both files read to the same AST\n" },
    { "demangle", cmd_demangle, 0,
      "decode mangled C symbols back to Turmeric names",
      "  tur demangle [<name>...]          decode mangled C symbols back to Turmeric names\n"
      "                                    (filters stdin when given no names, so\n"
      "                                     `perf script | tur demangle` reads clean)\n" },
    { "smt", cmd_smt, 0,
      "run an SMT-LIB2 script through the refinement solver",
      "  tur smt <file.smt2>               run an SMT-LIB2 script through the refinement solver\n" },
    { "experiments", cmd_experiments, 0,
      "list experimental features",
      "  tur experiments                   list experimental features (--enable=<name>)\n" },
    { "dialects", cmd_dialects, 0,
      "list the base dialects a #lang line may name",
      "  tur dialects                      list the base dialects a `#lang` line may name\n" },
    { "completion", run_completion, DRV_CMD_EXACT,
      "print a shell completion script",
      "  tur completion <zsh|bash>         print a shell completion script\n" },

    /* Package management (Spice, Phase PKG-1). */
    { "init", cmd_pkg_init, DRV_CMD_PACKAGE,
      "create a new project in the current directory",
      "  tur init [--bin|--lib] <name>     create a new project\n" },
    { "add", cmd_pkg_add, DRV_CMD_PACKAGE,
      "add a Turmeric spice dependency",
      "  tur add <url> [--ref <tag>]       add a Turmeric spice\n"
      "  tur add <path> --path             add a local spice\n"
      "  tur add --workspace <name>        assert a workspace sibling (no manifest entry)\n" },
    { "add-cmake", cmd_pkg_add_cmake, DRV_CMD_PACKAGE,
      "add a C/CMake dependency",
      "  tur add-cmake <url> [--ref <tag>] add a C/CMake dependency\n" },
    { "fetch", cmd_pkg_fetch, DRV_CMD_PACKAGE,
      "download or update declared spices",
      "  tur fetch [--update|--frozen|--dry-run|--refetch]  download/update spices (--frozen: fail if tur.lock would change; --refetch bypasses system pkgs)\n" },
    { "audit", cmd_pkg_audit, DRV_CMD_PACKAGE,
      "list every origin the build fetches code from",
      "  tur audit                         list every origin the build fetches code from\n" },
    { "emit-cmake", cmd_pkg_emit_cmake, DRV_CMD_PACKAGE,
      "generate CMakeLists.txt for CMake consumers",
      "  tur emit-cmake [--output-dir <d>] generate CMakeLists.txt + config for CMake consumers\n" },
    /* GS-M2: global spice installs. */
    { "install", cmd_pkg_install, DRV_CMD_PACKAGE,
      "install a spice binary globally",
      "  tur install <url> [--ref <ref>]   install a spice binary globally\n"
      "  tur install <path> --path         install a local spice binary globally\n" },
    { "uninstall", cmd_pkg_uninstall, DRV_CMD_PACKAGE,
      "remove a globally installed spice",
      "  tur uninstall <name>              remove a globally installed spice\n" },
    /* GS-M3 / GS-M4 */
    { "list", cmd_pkg_list, DRV_CMD_PACKAGE,
      "list globally installed spices",
      "  tur list [--verbose|--outdated]   list globally installed spices\n" },
    { "upgrade", cmd_pkg_upgrade, DRV_CMD_PACKAGE,
      "upgrade globally installed spices",
      "  tur upgrade <name>...|--all       upgrade globally installed spices\n" },

    /* Not in `tur --help`. */
    { "new", cmd_pkg_new, DRV_CMD_PACKAGE, "scaffold a new spice", NULL },
    { "audit-spans", cmd_audit_spans_main, 0, "report source-span coverage", NULL },
    { "lsp", cmd_lsp_main, 0, "Language Server Protocol server", NULL },
    { "mcp", cmd_mcp_main, 0, "Model Context Protocol server", NULL },
    { "image-info", cmd_image_info_main, 0, "print metadata for a compiled image", NULL },
    { "image-verify", cmd_image_verify_main, 0, "verify a compiled image", NULL },
    { "jit", cmd_jit, DRV_CMD_EXACT,
      "build and run a file in process through the MIR JIT", NULL },
    { "emit-rt-split", cmd_emit_rt_split, DRV_CMD_EXACT, NULL, NULL },
};

/* Other spellings of a command.  `tur just` is `tur run`: the task-runner mode
 * reads a Justfile, so `just` is the name muscle memory reaches for.  An alias
 * takes part in prefix resolution like a command, and resolving to its own
 * command is not an ambiguity (`tur ru` is `run`).  `--interpret`, spelled as
 * a flag, is matched exactly: no token starting with '-' is prefix-resolved. */
static const struct { const char *alias; const char *name; const char *summary; }
COMMAND_ALIASES[] = {
    { "just", "run", "synonym for run" },
    { "--interpret", "interpret", NULL },
};

#define N_COMMANDS (sizeof COMMANDS / sizeof COMMANDS[0])
#define N_ALIASES  (sizeof COMMAND_ALIASES / sizeof COMMAND_ALIASES[0])

static const DrvCommand *command_named(const char *name) {
    for (size_t i = 0; i < N_COMMANDS; i++)
        if (strcmp(name, COMMANDS[i].name) == 0) return &COMMANDS[i];
    return NULL;
}

const DrvCommand *drv_find_command(const char *tok, bool *ambiguous) {
    *ambiguous = false;
    if (!tok || !*tok) return NULL;
    /* An exact spelling wins outright. */
    const DrvCommand *c = command_named(tok);
    if (c) return c;
    for (size_t i = 0; i < N_ALIASES; i++)
        if (strcmp(tok, COMMAND_ALIASES[i].alias) == 0)
            return command_named(COMMAND_ALIASES[i].name);
    /* Then a unique prefix of a command or an alias. */
    if (tok[0] == '-') return NULL;
    size_t n = strlen(tok);
    const DrvCommand *hit = NULL;
    for (size_t i = 0; i < N_COMMANDS; i++) {
        if (COMMANDS[i].flags & DRV_CMD_EXACT) continue;
        if (strncmp(tok, COMMANDS[i].name, n) == 0) {
            if (hit) { *ambiguous = true; return NULL; }
            hit = &COMMANDS[i];
        }
    }
    for (size_t i = 0; i < N_ALIASES; i++) {
        if (COMMAND_ALIASES[i].alias[0] == '-') continue;
        if (strncmp(tok, COMMAND_ALIASES[i].alias, n) == 0) {
            const DrvCommand *to = command_named(COMMAND_ALIASES[i].name);
            if (hit && hit != to) { *ambiguous = true; return NULL; }
            hit = to;
        }
    }
    return hit;
}

/* True when `name` is a built-in subcommand in any spelling dispatch accepts. */
static bool is_builtin_command(const char *name) {
    if (command_named(name)) return true;
    for (size_t i = 0; i < N_ALIASES; i++)
        if (strcmp(name, COMMAND_ALIASES[i].alias) == 0) return true;
    return false;
}

/* `tur completion`: the scripts list every command with a summary, and the
 * aliases that have one, in table order. */
static int run_completion(int argc, char **argv) {
    CompletionCommand list[N_COMMANDS + N_ALIASES];
    int n = 0;
    for (size_t i = 0; i < N_COMMANDS; i++) {
        if (!COMMANDS[i].summary) continue;
        list[n].name = COMMANDS[i].name;
        list[n].summary = COMMANDS[i].summary;
        n++;
        for (size_t k = 0; k < N_ALIASES; k++) {
            if (!COMMAND_ALIASES[k].summary ||
                strcmp(COMMAND_ALIASES[k].name, COMMANDS[i].name) != 0) continue;
            list[n].name = COMMAND_ALIASES[k].alias;
            list[n].summary = COMMAND_ALIASES[k].summary;
            n++;
        }
    }
    return cmd_completion(argc, argv, list, n);
}

/* GS-M2: scan $PATH for `tur-*` executables and print them as an
 * "External commands:" block beneath the built-in usage. Built-in
 * subcommands always win at dispatch time, so we filter any `tur-<name>`
 * that shadows a built-in to avoid misleading the user. Within $PATH,
 * the first hit for each name wins (matching exec() resolution). */
static void list_external_subcommands(void) {
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) return;

    char **names = NULL;
    int n_names = 0, cap_names = 0;

    const char *p = path_env;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t seg = colon ? (size_t)(colon - p) : strlen(p);
        if (seg > 0 && seg < 3500) {
            char dir[4096];
            snprintf(dir, sizeof(dir), "%.*s", (int)seg, p);
            DIR *d = opendir(dir);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (strncmp(de->d_name, "tur-", 4) != 0) continue;
                    const char *cmd = de->d_name + 4;
                    if (!*cmd) continue;
                    for (const char *q = cmd; *q; q++) {
                        if (!(isalnum((unsigned char)*q) || *q == '-' || *q == '_'))
                            goto skip;
                    }
                    if (is_builtin_command(cmd)) continue;
                    {
                        char full[8192];
                        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
                        if (access(full, X_OK) != 0) continue;
                    }
                    {
                        bool dup = false;
                        for (int i = 0; i < n_names; i++) {
                            if (strcmp(names[i], cmd) == 0) { dup = true; break; }
                        }
                        if (dup) continue;
                    }
                    if (n_names == cap_names) {
                        int nc = cap_names ? cap_names * 2 : 8;
                        char **nn = (char **)realloc(names, (size_t)nc * sizeof(char *));
                        if (!nn) { closedir(d); goto cleanup; }
                        names = nn;
                        cap_names = nc;
                    }
                    names[n_names++] = strdup(cmd);
                  skip: ;
                }
                closedir(d);
            }
        }
        if (!colon) break;
        p = colon + 1;
    }

    if (n_names == 0) goto cleanup;

    for (int i = 1; i < n_names; i++) {
        char *cur = names[i];
        int j = i;
        while (j > 0 && strcmp(names[j - 1], cur) > 0) {
            names[j] = names[j - 1];
            j--;
        }
        names[j] = cur;
    }

    fprintf(stderr, "\nexternal commands (tur-* on $PATH):\n");
    for (int i = 0; i < n_names; i++)
        fprintf(stderr, "  tur %s\n", names[i]);

cleanup:
    for (int i = 0; i < n_names; i++) free(names[i]);
    free(names);
}

int drv_usage(void) {
    fprintf(stderr,
        "tur: the Turmeric compiler (v" TUR_VERSION ")\n"
        "\n"
        "usage:\n");
    for (size_t i = 0; i < N_COMMANDS; i++)
        if (COMMANDS[i].help && !(COMMANDS[i].flags & DRV_CMD_PACKAGE))
            fputs(COMMANDS[i].help, stderr);
    fprintf(stderr,
        "\n"
        "package management (Spice, Phase PKG-1):\n");
    for (size_t i = 0; i < N_COMMANDS; i++)
        if (COMMANDS[i].help && (COMMANDS[i].flags & DRV_CMD_PACKAGE))
            fputs(COMMANDS[i].help, stderr);
    fprintf(stderr,
        "\n"
        "emit-c flags:\n"
        "  tur emit-c <file>                 compile a .tur file to C (stdout)\n"
        "  tur emit-c --output-dir <dir> <files...>  compile each .tur to <dir>/<mod>.h + .c\n"
        "\n"
        "global flags:\n"
        "  --enable=<name>[,<name>...]      turn on an experimental feature; see 'tur experiments'\n"
        "  --macro-caps=io                  grant macro-time (defmacro*) code I/O; default deny\n"
        "  --no-proc-macros                 refuse defmacro* instead of running it (untrusted trees)\n"
        "  --no-color                       disable colored diagnostics\n"
        "  --json                           structured JSON output (tur doc, tur test, tur check)\n"
        "  --json-diagnostics               output diagnostics as JSON (phase 8)\n"
        "  --explain <TUR-E####>            print explanation for a diagnostic code (HKT-P5)\n"
        "  --explain <snippet>              compile code snippet and explain errors (phase 8)\n"
        "  --timings[=<path>]               time each compiler phase (read, elaborate, emit, cc, ...):\n"
        "                                   a table on stderr; with a path, tur-profile/1 JSON there too\n"
        "                                   (`--timings <path>` when it ends in .json or is /dev/...)\n"
        "  --dump-kinds                     dump kind annotations after kind-check (HKT-P6)\n"
        "  --strict-effects                 warn on unannotated effectful functions (ER1)\n"
        "  -Werror=strict-effects           make the --strict-effects warnings errors (implies it)\n"
        "  --strict-refine                  hard-fail refinement obligations the solver cannot prove\n"
        "  --dump-effects                   print inferred effect row for each defn (ER6)\n"
        "  --dump-write-frames              print the checked verdict for each `#writes` frame (G1)\n"
        "  --dump-read-frames               print the verification verdict for each `#reads` frame (R4)\n"
        "  --dump-cps-coloring              print whole-program may-capture coloring per defn (CPS1)\n"
        "  --dump-cps                       print the ANF/CPS IR for each colored defn (CPS2)\n"
        "  --lint-effects                   deprecated alias for --strict-effects (TUR-W0050)\n"
        "  --backtrack-depth <N>            cap run-backtrack at N results (0=unlimited) (Phase B5)\n"
        "  --dump-clone-plan                dump cloneable capture plan after CPS (Phase B5)\n"
        "  --dump-cps-coloring              dump CPS coloring (colored/uncolored) per top-level defn (CPS1)\n"
        "  --cps-path                       emit CPS wrappers for colored functions (CPS3)\n"
        "  --emit-abi-trace                 print the resolved ABI path per call site during emit-c (Phase I)\n"
        "  --no-abi-cache                   disable the persistent cross-module ABI cache (.tur-abi-cache/) (Phase J6)\n"
        "  --no-r7rs-gc                     build a #lang r7rs program without its collector (TUR_R7RS_GC=0; for a program that starts threads)\n"
        "  --no-saffron-gc                  build a #lang saffron program without the collector (TUR_SAFFRON_GC=0)\n"
        "  --panic-abort                   all panics call abort() directly (Phase R5)\n"
        "  --no-auto-spice                  no enclosing-spice discovery: only -I dirs are searched\n"
        "  --no-global-spices               outside a project, do not search the installed spices\n"
        "                                   (TUR_NO_GLOBAL_SPICES=1; implied by --no-auto-spice)\n"
        "  --warn-unused-result             warn on discarded result values (Phase R6)\n"
        "  --no-warn-unused-result          disable --warn-unused-result (Phase R6)\n"
        "  --lint-panic                     lint panic/must! usage (Phase R6)\n"
        "  --no-contracts                   strip contract checks; predicates not evaluated (Phase C2)\n"
        "  --dump-sizes                     print inferred size index per sized-GADT constructor (SZ8)\n"
        "  --keep-contracts                 retain contract checks in release builds (CT3)\n"
        "  -X<name>                         recognized for backwards compatibility; all language\n"
        "                                   features previously gated by -X flags are now on by default.\n"
        "                                   Each -X<name> emits TUR-W0050 and is otherwise ignored.\n"
        "                                   See docs/guides/compiler-flags-guide.md for the removal list.\n");
    list_external_subcommands();
    return 64;
}

/* GS-M2: subcommand fallthrough. When `tur <cmd>` doesn't match a built-in,
 * search $PATH for `tur-<cmd>` and exec it with the remaining argv. Modeled
 * after git-foo / git foo. On success this does not return. On failure
 * prints a diagnostic and returns 1. */
int drv_try_external_subcommand(int argc, char **argv) {
    if (argc < 2) return drv_usage();
    const char *cmd = argv[1];
    if (!cmd || !*cmd || cmd[0] == '-') return drv_usage();
    /* Reject anything that's clearly a path or contains shell-unsafe bits. */
    for (const char *p = cmd; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) return drv_usage();
    }
    char ext_name[256];
    snprintf(ext_name, sizeof(ext_name), "tur-%s", cmd);

    /* The suffixes a command name can carry.  Windows resolves a bare name
     * through PATHEXT, and `tur install` writes `tur-foo.exe`, so probing only
     * the bare name finds nothing there. */
#ifdef _WIN32
    static const char *const EXTS[] = { ".exe", "" };
#else
    static const char *const EXTS[] = { "" };
#endif

    const char *path_env = getenv("PATH");
    if (path_env) {
        const char *p = path_env;
        while (*p) {
            /* TUR_PATH_LIST_SEP, not ':': on Windows a ':' split tears every
             * entry at its drive colon and no candidate is ever well formed. */
            const char *sep = strchr(p, TUR_PATH_LIST_SEP);
            size_t seg = sep ? (size_t)(sep - p) : strlen(p);
            for (size_t e = 0; seg > 0 && seg < 3500
                               && e < sizeof(EXTS) / sizeof(EXTS[0]); e++) {
                char candidate[4096];
                snprintf(candidate, sizeof(candidate), "%.*s/%s%s",
                         (int)seg, p, ext_name, EXTS[e]);
                if (access(candidate, X_OK) != 0) continue;
                char **nv = (char **)calloc((size_t)argc, sizeof(char *));
                if (!nv) return 1;
                nv[0] = (char *)ext_name;
                for (int i = 2; i < argc; i++) nv[i - 1] = argv[i];
                nv[argc - 1] = NULL;
#ifdef _WIN32
                /* _execv does not REPLACE the process on Windows: the parent
                 * exits immediately and a calling shell sees the command
                 * finish before the child has.  Wait and pass the status on. */
                intptr_t rc = _spawnv(_P_WAIT, candidate, (const char *const *)nv);
                if (rc >= 0) { free(nv); return (int)rc; }
#else
                execv(candidate, nv);
#endif
                fprintf(stderr, "tur: failed to exec '%s': %s\n",
                        candidate, strerror(errno));
                free(nv);
                return 1;
            }
            if (!sep) break;
            p = sep + 1;
        }
    }
    fprintf(stderr,
        "tur: '%s' is not a tur command. See 'tur --help'.\n", cmd);
    return 1;
}
