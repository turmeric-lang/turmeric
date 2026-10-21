/* cli.c -- the command line: the per-subcommand usage text, `-I` parsing, the
 * `-X` deprecation and `--enable=` helpers, and the `--no-auto-*` flags.
 *
 * Part of the compiler driver (src/driver/); driver.h says how the files
 * share declarations. */

#include "driver.h"
#include "commands.h"

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

void warn_deprecated_x_flag(const char *name) {
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
        "                       for build.tur and adds <spice>/src to the path)\n"
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
        "  tur repl [--watch] [--lang <dialect>] [--engine <name>]\n"
        "                  start the interactive REPL\n"
        "\n"
        "flags:\n"
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
        "  :tutorial       start the interactive tutorial\n"
        "\n"
        "REPL forms:\n"
        "  (reload)        rebuild the loaded spice and refresh bindings\n"
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

/* --no-color, read by the dispatcher's scan; the interpreter commands pass
 * it on as their use_color. */
bool g_no_color = false;
