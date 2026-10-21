/* tur: the Turmeric compiler driver (phase 2). */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* _DARWIN_C_SOURCE exposes mkstemps and DT_REG on macOS without suppressing
 * POSIX extensions; on Linux _DEFAULT_SOURCE covers both. */
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#elif !defined(_WIN32)
/* Windows is excluded deliberately: MinGW reads _POSIX_C_SOURCE as "hide the
 * Win32 CRT names", which un-declares mkdir/getcwd and hides _finddata_t --
 * which in turn breaks <dirent.h> itself.  glibc needs this macro to EXPOSE
 * those declarations; on Windows it does the exact opposite. */
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <ctype.h>
#include "source_literal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#ifndef _WIN32
/* poll(2) has no Windows counterpart for CRT file descriptors; the only user is
 * the fork-based fixture worker, which is compiled out below. */
#include <poll.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include "platform_proc.h"
#include "tur_argcheck.h"
#include "platform_fs.h"  /* realpath/mkdir/setenv/mkstemps/... on Windows */
#ifdef _WIN32
#include <io.h>       /* _setmode, _fileno */
#endif
#ifndef _WIN32
/* waitpid() is only used by the fork-based fixture worker, which is compiled out
 * on Windows.  The WIFEXITED/WEXITSTATUS uses that remain are applied to a
 * system() return value, and platform_fs.h defines those there. */
#include <sys/wait.h>
#include <sys/time.h>   /* utimes: the prelude cache's LRU stamp */
/* The docs server (cmd_docs_serve), which Windows does not build. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>     /* SN1: _NSGetExecutablePath for stdlib resolution */
#endif

#include "arena.h"
#include "assert.h"
#include "buf.h"
#include "borrow_check.h"  /* Phase 14 */
#include "cps.h"          /* Phase 18: CPS transformation */
#include "srfi_prune.h"   /* r7rs-srfi-plan S3: unreached SRFI definitions */
#include "jit_prune.h"    /* JIT: drop the prelude a program never reaches */
#include "cps_ir.h"       /* CPS2: ANF/CPS IR (--dump-cps) */
#include "diag.h"
#include "effect_check.h" /* Phase P19-2: effect-row inference */
#include "effect.h"       /* built-in effect registration */
#include "kind_check.h"   /* Phase HKT H0: kind inference pass */
#include "compiler/refine_smtlib.h" /* SX8a: `tur smt` -- the SMT-LIB2 seam */
#include "compiler/refine_solver.h" /* SX8a: the S0..S3 chain `tur smt` runs */
#include "elab.h"
#include "emit.h"
#include "compiler/emit_split.h" /* r7rs-programs-compile-slowly */
#include "compiler/stack_guard.h" /* tur_run_on_big_stack -- see the note at EOF */
#include "runtime/hamt.h" /* S2: tur_hamt_hash_xxh64 for the split-artifact hash */
#include "runtime/image.h" /* AI6: tur image-info / image-verify */
#include "runtime/rt_split_embed.h" /* S2: committed decls region + hash (TUR_JIT) */
#include "turi/spice_loader.h" /* J2: the REPL's in-process jit hook */
#include "turi/jit_ffi.h"      /* jit-ffi-c2mir-plan: dynamic-FFI provider */
#include "turi/inline_c_jit.h"  /* aot-compiled-repl-plan C1 */
#include "effect_lower.h" /* Phase 19: Effect lowering */
#include "expr.h"
#include "fmt.h"
#include "forms.h"
#include "pass.h"         /* Phase P19-1: pass scheduling */
#include "prof.h"         /* profiling-plan C0+C1: --timings */
#include "reader.h"
#include "reader_macros.h"
#include "lang_dialects.h"  /* `tur dialects`: the `#lang` base axis */
#include "refine_discharge.h" /* RT3: per-compile refinement stats reset */
#include "span_audit.h"  /* debugger Phase 1: breakpoint-span coverage audit */
#include "symbols.h"
/* Phase S0: eval API for tur repl */
#include "turi/eval.h"
#include "turi/collections_native.h"
#include "turi/interpreter_natives.h"
#include "turi/docstrings.h"
#include "turi/preload.h"
/* Phase S1: REPL with libedit, multi-line input, :type/:doc/:reload */
#include "turi/repl.h"
/* Phase PKG-1: Spice package manager */
#include "pkg.h"
#include "global.h"   /* tur_installed_spice_dir: :global spice deps */
#include "spice_search.h"  /* find_spice_root / auto_append_spice_includes */
/* RN0-RN7: Justfile-compatible task runner */
#include "justrun.h"
/* Global configuration variables — defined in globals.c */
#include "globals.h"
#include "experiments.h"  /* XF1: --enable=<name> experimental-flag registry */
#include "jit_engine.h"   /* J1: tur jit (TUR_HAVE_JIT builds only) */
#include "mono_specs.h"   /* VBM1: --dump-mono-specs registry dump */
/* LSP server */
#include "lsp/lsp.h"
#include "lsp/lsp_sym.h"
#include "lsp/lsp_collect.h"
#include "lsp/lsp_scope.h"
#include "stdlib_autoload.h"
#include "lsp/lsp_docs.h"
/* MCP server */
#include "lsp/mcp.h"
/* DAP server (debugger Phase 3) */
#include "turi/dap.h"
#include "turi/trace.h"
/* lsp-lite: lightweight completion/calltip backend for editors */
#include "cli/lsp_lite.h"
/* tur completion <zsh|bash>: emit a shell completion script */
#include "cli/completion.h"
#include "cli/demangle.h"
/* The driver's shared declarations (main-c-split-plan). */
#include "driver/driver.h"
#include "driver/commands.h"

/* True when a `--help` / `-h` appears among the subcommand's arguments.  For
 * the subcommands whose argv loop has no explicit help arm (emit-c, emit-h),
 * so that `--help` keeps exiting 0 rather than falling into the unknown-flag
 * arm, which now exits 2. */
static bool has_help_flag(int argc, char **argv) {
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) return true;
    }
    return false;
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
static void parse_timings_flag(int *argc_io, char **argv) {
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

/* Top-level subcommands, in canonical spelling. Kept in one place so
 * resolve_command() can prefix-match against them; the actual dispatch
 * chain in main() still uses strcmp against these same names. */
static const char *const CANONICAL_COMMANDS[] = {
    "emit-c", "emit-h", "emit-cmake", "check", "expand", "audit-spans",
    "lsp", "mcp", "dap", "trace", "lsp-lite",
    "build", "compile", "link", "run", "repl", "worker", "interpret", "debug",
    "eval", "doc", "docs", "image-info", "image-verify", "explain",
    "format", "fmt", "parse-check", "test", "demangle",
    "new", "init", "add", "add-cmake", "fetch", "audit",
    "install", "uninstall", "list", "upgrade", "experiments", "dialects",
    "smt",
    NULL,
};

/* Built-in subcommands that dispatch on their exact spelling only.  They stay
 * out of CANONICAL_COMMANDS[] because prefix resolution would change under
 * them: `completion` would make `tur comp` (today `compile`) ambiguous, and
 * `jit` would do the same to `tur j` (today the `just` alias of `run`).
 * `emit-rt-split` is the generator tools/gen-runtime-split.py runs, not a
 * command anyone types.  They still shadow a `tur-<name>` on $PATH. */
static const char *const HIDDEN_COMMANDS[] = {
    "jit", "completion", "emit-rt-split",
    NULL,
};

/* Alternate spellings that dispatch to a canonical command.  `tur just` is a
 * synonym for `tur run`: the task-runner mode reads a Justfile, so `just` is
 * the name muscle memory reaches for.  Aliases participate in the same
 * prefix resolution as canonical names, and resolve straight to the
 * canonical spelling so every downstream dispatch arm sees one name. */
static const struct { const char *alias; const char *canonical; }
COMMAND_ALIASES[] = {
    { "just", "run" },
    { NULL,   NULL  },
};

/* Sentinel: `tok` is a prefix of more than one canonical command. */
static const char *const COMMAND_AMBIGUOUS = (const char *)(intptr_t)-1;

/* True when `name` is a built-in subcommand in any spelling dispatch accepts:
 * canonical, hidden, or an alias. */
static bool is_builtin_command(const char *name) {
    for (int i = 0; CANONICAL_COMMANDS[i] != NULL; i++)
        if (strcmp(name, CANONICAL_COMMANDS[i]) == 0) return true;
    for (int i = 0; HIDDEN_COMMANDS[i] != NULL; i++)
        if (strcmp(name, HIDDEN_COMMANDS[i]) == 0) return true;
    for (int i = 0; COMMAND_ALIASES[i].alias != NULL; i++)
        if (strcmp(name, COMMAND_ALIASES[i].alias) == 0) return true;
    return false;
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

/* Resolve a bare argv[1] token to a canonical command name.
 *   - exact match wins outright.
 *   - unique prefix -> canonical.
 *   - >1 prefix hits -> COMMAND_AMBIGUOUS.
 *   - flags (leading '-') and 0 hits -> NULL. */
static const char *resolve_command(const char *tok) {
    if (!tok || !*tok || tok[0] == '-') return NULL;
    for (int i = 0; CANONICAL_COMMANDS[i] != NULL; i++) {
        if (strcmp(tok, CANONICAL_COMMANDS[i]) == 0) return CANONICAL_COMMANDS[i];
    }
    for (int i = 0; COMMAND_ALIASES[i].alias != NULL; i++) {
        if (strcmp(tok, COMMAND_ALIASES[i].alias) == 0)
            return COMMAND_ALIASES[i].canonical;
    }
    size_t n = strlen(tok);
    const char *hit = NULL;
    for (int i = 0; CANONICAL_COMMANDS[i] != NULL; i++) {
        if (strncmp(tok, CANONICAL_COMMANDS[i], n) == 0) {
            if (hit != NULL) return COMMAND_AMBIGUOUS;
            hit = CANONICAL_COMMANDS[i];
        }
    }
    for (int i = 0; COMMAND_ALIASES[i].alias != NULL; i++) {
        if (strncmp(tok, COMMAND_ALIASES[i].alias, n) == 0) {
            /* An alias and its own canonical name are not two commands, so
             * `tur ru` stays `run` rather than becoming ambiguous. */
            if (hit != NULL && strcmp(hit, COMMAND_ALIASES[i].canonical) != 0)
                return COMMAND_AMBIGUOUS;
            hit = COMMAND_ALIASES[i].canonical;
        }
    }
    return hit;
}

static int usage(void) {
    fprintf(stderr,
        "tur: the Turmeric compiler (v" TUR_VERSION ")\n"
        "\n"
        "usage:\n"
        "  tur build <file.tur> [-o <out>]    build a single file\n"
        "  tur build <dir> [-o <out>]         build all .tur files in directory\n"
        "  tur compile <file.tur> -o <out.o>  lower a .tur to an object (+ .link sidecar)\n"
        "  tur link <obj/src>... -o <out>     link objects (+ .link sidecars) into an exe\n"
        "  tur emit-c <input.tur>            print the generated C to stdout\n"
        "  tur emit-h <input.tur>            print the generated header to stdout\n"
        "  tur run <input.tur>               build + execute a single file\n"
        "  tur run <recipe>                  run a Justfile recipe (`tur just` is a synonym)\n"
        "  tur repl                          interactive REPL (Phase S1)\n"
        "  tur worker                        persistent fixture evaluator (Tier 3, reads dirs from stdin)\n"
        "  tur interpret [-I dir] <file.tur> run a file through the tree-walking interpreter\n"
        "  tur debug [-I dir] <file.tur>     run a file under the interactive debugger\n"
        "  tur dap                           Debug Adapter Protocol server (JSON-RPC/stdio) for editors\n"
        "  tur trace <file.tur> [-o f]       record an interpreted run; --dump reads one back\n"
        "  tur lsp-lite                      lightweight completion/calltip/doc backend (NDJSON/stdio)\n"
        "  tur eval '<expr>'                 evaluate an inline expression\n"
        "  tur doc <symbol>                  print documentation for a builtin, special form, or stdlib definition\n"
        "  tur docs [--open|--serve]         locate, open, or serve the rendered guides and API reference\n"
        "  tur explain <TUR-E####|snippet>   explain a diagnostic code or snippet errors\n"
        "  tur test <dir>                    run all .tur files in a directory\n"
        "  tur check <input.tur>             type-check only, no codegen (phase 8)\n"
        "  tur expand <input.tur>            trace macro expansions to stdout while checking\n"
        "  tur format [--check|--diff] [file.tur]   format source (stdin if no file given)\n"
        "  tur fmt [--check|--diff|--dry-run] [paths...]  format in place with dir walking\n"
        "  tur parse-check <a> <b>           exit 0 if both files read to the same AST\n"
        "  tur demangle [<name>...]          decode mangled C symbols back to Turmeric names\n"
        "                                    (filters stdin when given no names, so\n"
        "                                     `perf script | tur demangle` reads clean)\n"
        "  tur smt <file.smt2>               run an SMT-LIB2 script through the refinement solver\n"
        "  tur check --dump-refine=json <f>  print one JSON record per refinement obligation\n"
        "  tur experiments                   list experimental features (--enable=<name>)\n"
        "  tur dialects                      list the base dialects a `#lang` line may name\n"
        "  tur completion <zsh|bash>         print a shell completion script\n"
        "\n"
        "package management (Spice, Phase PKG-1):\n"
        "  tur init [--bin|--lib] <name>     create a new project\n"
        "  tur add <url> [--ref <tag>]       add a Turmeric spice\n"
        "  tur add <path> --path             add a local spice\n"
        "  tur add --workspace <name>        assert a workspace sibling (no manifest entry)\n"
        "  tur add-cmake <url> [--ref <tag>] add a C/CMake dependency\n"
        "  tur fetch [--update|--frozen|--dry-run|--refetch]  download/update spices (--frozen: fail if tur.lock would change; --refetch bypasses system pkgs)\n"
        "  tur audit                         list every origin the build fetches code from\n"
        "  tur emit-cmake [--output-dir <d>] generate CMakeLists.txt + config for CMake consumers\n"
        "  tur install <url> [--ref <ref>]   install a spice binary globally\n"
        "  tur install <path> --path         install a local spice binary globally\n"
        "  tur uninstall <name>              remove a globally installed spice\n"
        "  tur list [--verbose|--outdated]   list globally installed spices\n"
        "  tur upgrade <name>...|--all       upgrade globally installed spices\n");
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

/* The interpreter arms (`tur --interpret`, `tur debug`): collect the `-I`
 * flags that come BEFORE the file path, starting at argv[start].  Everything
 * after the path is the program's *args*, untouched -- a `-I` there belongs to
 * the program.  Returns the index of the file path (argc when there is none),
 * or -1 after reporting a bare trailing `-I`.  *out_dirs is malloc'd (caller
 * frees); its strings are borrowed from argv. */
static int interp_leading_include_flags(int argc, char **argv, int start,
                                        char ***out_dirs, int *n_out) {
    *out_dirs = NULL;
    *n_out = 0;
    int i = start;
    while (i < argc) {
        int consumed = 0;
        if (!drv_is_include_flag(argc, argv, i, &consumed)) {
            if (strcmp(argv[i], "-I") == 0) {
                free(*out_dirs);
                *out_dirs = NULL;
                *n_out = 0;
                fprintf(stderr, "tur: -I requires a directory argument\n");
                return -1;
            }
            break;
        }
        char **bigger = (char **)realloc(*out_dirs,
                                         (size_t)(*n_out + 1) * sizeof(char *));
        if (!bigger) break;
        *out_dirs = bigger;
        (*out_dirs)[(*n_out)++] = consumed == 2 ? argv[i + 1] : argv[i] + 2;
        i += consumed;
    }
    return i;
}

/* Phase 8: Handle --no-color flag */
static bool parse_no_color(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-color") == 0) {
            return true;
        }
    }
    return false;
}

static bool parse_no_auto_spice(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-auto-spice") == 0) return true;
    }
    return false;
}

static bool parse_no_abi_cache(int argc, char **argv) {
    const char *env = getenv("TUR_NO_ABI_CACHE");
    if (env && *env && strcmp(env, "0") != 0) return true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-abi-cache") == 0) return true;
    }
    return false;
}

/* r7rs-gc (graduated 2026-09-25): the collector is the allocator of every
 * compiled single-unit `#lang r7rs` program (g_opt_r7rs_gc defaults true).
 * `TUR_R7RS_GC=0` or `--no-r7rs-gc` builds one without it -- the way out for
 * a program that starts threads, which the collector refuses at the start
 * site (docs/archive/r7rs-gc-plan.md).  `TUR_R7RS_GC=1` is the default
 * spelled out (tests/run-r7rs-gc.sh's "with" arm). */
/* any-widen-stored-in-an-adt-field-has-no-owner: the same collector is the
 * allocator of every compiled single-unit `#lang saffron` program
 * (g_opt_saffron_gc defaults true); `TUR_SAFFRON_GC=0` or `--no-saffron-gc`
 * builds one on plain malloc/free, where its `any` boxes are owned only as far
 * as the static drops reach. */
static bool parse_no_saffron_gc(int argc, char **argv) {
    bool off = false;
    const char *env = getenv("TUR_SAFFRON_GC");
    if (env && env[0] == '0') off = true;
    else if (env && env[0] == '1') off = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-saffron-gc") == 0) off = true;
    }
    return off;
}

static bool parse_no_r7rs_gc(int argc, char **argv) {
    bool off = false;
    const char *env = getenv("TUR_R7RS_GC");
    if (env && env[0] == '0') off = true;
    else if (env && env[0] == '1') off = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-r7rs-gc") == 0) off = true;
    }
    return off;
}

/* Phase R5: Handle --panic-abort flag */
static bool parse_panic_abort(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-abort") == 0) {
            return true;
        }
    }
    return false;
}

/* Debugger Phase 4: --debug emits `#line` source-map directives into the
 * generated C and builds single-file targets with `-g -O0`. */
static bool parse_debug_build(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase R6: Handle --warn-unused-result flag */
static bool parse_warn_unused_result(int argc, char **argv) {
    bool enabled = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--warn-unused-result") == 0) {
            enabled = true;
        } else if (strcmp(argv[i], "--no-warn-unused-result") == 0) {
            enabled = false;
        }
    }
    return enabled;
}

/* F4 (cross-plan-followups): --Werror=deprecated -- what it promotes is
 * listed at g_werror_deprecated (runtime/globals.c). */
static bool parse_werror_deprecated(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--Werror=deprecated") == 0 ||
            strcmp(argv[i], "-Werror=deprecated") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase C: --Werror=inline-c-narrow-params promotes narrow-param-in-inline-C
 * warnings to hard errors so a strict build can gate against unannotated
 * narrow parameters reaching inline-C bodies. */
static bool parse_werror_inline_c_narrow_params(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--Werror=inline-c-narrow-params") == 0 ||
            strcmp(argv[i], "-Werror=inline-c-narrow-params") == 0) {
            return true;
        }
    }
    return false;
}

/* -Werror=strict-effects promotes the --strict-effects lints (TUR-W0030,
 * TUR-W0032) to errors.  It implies --strict-effects; main applies that. */
static bool parse_werror_strict_effects(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--Werror=strict-effects") == 0 ||
            strcmp(argv[i], "-Werror=strict-effects") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase R6: Handle --lint-panic flag */
static bool parse_lint_panic(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lint-panic") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase C2: Handle --no-contracts flag */
static bool parse_no_contracts(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-contracts") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase 8: Handle --json-diagnostics flag */
static bool use_json_diagnostics = false;

/* GS-M2: subcommand fallthrough. When `tur <cmd>` doesn't match a built-in,
 * search $PATH for `tur-<cmd>` and exec it with the remaining argv. Modeled
 * after git-foo / git foo. On success this does not return. On failure
 * prints a diagnostic and returns 1. */
static int try_external_subcommand(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    if (!cmd || !*cmd || cmd[0] == '-') return usage();
    /* Reject anything that's clearly a path or contains shell-unsafe bits. */
    for (const char *p = cmd; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) return usage();
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

static int tur_main_inner(int argc, char **argv) {
    /* profiling-plan C0+C1: --timings first, so the clock starts before
     * anything it should measure, and stripped before g_main_argv is taken
     * below -- the split build's cache key hashes the command line, and a
     * profiler that changed the key would profile a cold cache. */
    parse_timings_flag(&argc, argv);
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

    /* global-flags-parsed-after-double-dash: everything from the first `--`
     * on belongs to the program, so the global flags are parsed and stripped
     * from the words before it only.  `argc` is cut there for the parse, and
     * the tail (the `--` included, which the commands that forward arguments
     * look for) is put back after the strip loop below.  The strip only moves
     * words below the cut, so the tail is never touched in between. */
    int dd_at = argc;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--") == 0) { dd_at = i; break; }
    const int dd_tail = argc - dd_at;
    argc = dd_at;

    /* Phase 8: Check for global flags before command */
    bool no_color = parse_no_color(argc, argv);
    bool explain_mode = false;
    const char *explain_code = NULL;
    g_panic_abort = parse_panic_abort(argc, argv);
    g_warn_unused_result = parse_warn_unused_result(argc, argv);
    g_lint_panic = parse_lint_panic(argc, argv);
    /* Phase C2: --no-contracts strips contract checks (release builds). */
    g_no_contracts = parse_no_contracts(argc, argv);
    /* Debugger Phase 4: --debug emits `#line` directives + builds with -g -O0. */
    g_emit_debug_lines = parse_debug_build(argc, argv);
    /* F4: --Werror=deprecated -- see g_werror_deprecated (runtime/globals.c) */
    g_werror_deprecated = parse_werror_deprecated(argc, argv);
    /* Phase C: --Werror=inline-c-narrow-params promotes narrow-param warnings */
    g_werror_inline_c_narrow_params = parse_werror_inline_c_narrow_params(argc, argv);
    /* -Werror=strict-effects: promote the effect-annotation lints to errors. */
    g_werror_strict_effects = parse_werror_strict_effects(argc, argv);
    if (g_werror_strict_effects) g_strict_effects = true;
    /* SC4: --no-auto-spice disables enclosing-spice auto-discovery in
     * per-file subcommands (check/emit-c/emit-h/run). */
    g_no_auto_spice = parse_no_auto_spice(argc, argv);
    /* J6: --no-abi-cache / TUR_NO_ABI_CACHE disables the persistent
     * cross-module ABI specialization cache (.tur-abi-cache/). */
    g_no_abi_cache = parse_no_abi_cache(argc, argv);
    /* r7rs-gc: TUR_R7RS_GC=0 / --no-r7rs-gc builds a Scheme program without
     * the collector (a program that starts threads). */
    if (parse_no_r7rs_gc(argc, argv)) g_opt_r7rs_gc = false;
    if (parse_no_saffron_gc(argc, argv)) g_opt_saffron_gc = false;
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

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-abort") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--no-auto-spice") == 0) {
            /* SC4: already parsed into g_no_auto_spice; strip from argv. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--no-r7rs-gc") == 0 ||
                   strcmp(argv[i], "--no-saffron-gc") == 0) {
            /* r7rs-gc: already parsed into g_opt_r7rs_gc / g_opt_saffron_gc;
             * strip from argv. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--panic-trace") == 0) {
            /* Retired (panic-location-names-the-runtime-not-the-call-site):
             * it printed the defer-frame chain as raw pointers, which named
             * no function and no line.  A panic now names its own .tur file
             * and line, and `--debug` builds for a debugger's backtrace.
             * Still accepted so existing scripts keep working; TUR-W0050
             * says so. */
            fprintf(stderr, "warning [TUR-W0050]: --panic-trace is retired and "
                            "does nothing; a panic already names its .tur file "
                            "and line, and a --debug build gives a debugger "
                            "backtrace (`tur debug`)\n");
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--warn-unused-result") == 0 ||
                   strcmp(argv[i], "--no-warn-unused-result") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--Werror=deprecated") == 0 ||
                   strcmp(argv[i], "-Werror=deprecated") == 0) {
            /* F4: already parsed into g_werror_deprecated; remove from argv. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--Werror=inline-c-narrow-params") == 0 ||
                   strcmp(argv[i], "-Werror=inline-c-narrow-params") == 0) {
            /* Phase C: already parsed into g_werror_inline_c_narrow_params; remove. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--Werror=strict-effects") == 0 ||
                   strcmp(argv[i], "-Werror=strict-effects") == 0) {
            /* Already parsed into g_werror_strict_effects; remove. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--lint-panic") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--no-contracts") == 0) {
            /* Phase C2: already parsed into g_no_contracts; remove from argv. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--debug") == 0) {
            /* Debugger Phase 4: already parsed into g_emit_debug_lines; strip
             * so the per-command flag parsers (build/emit-c) don't reject it. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--json-diagnostics") == 0) {
            use_json_diagnostics = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--json") == 0) {
            /* E14: structured JSON output — implies --json-diagnostics for check */
            use_json_output = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--explain") == 0 && i + 1 < argc) {
            explain_mode = true;
            explain_code = argv[i + 1];
            /* Remove --explain and code from argv */
            for (int j = i; j < argc - 2; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        } else if (strcmp(argv[i], "--lint-unsafe") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_warn_nested = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--lint-unsafe-max-lines=", 24) == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_max_lines = (uint32_t)atoi(argv[i] + 24);
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--require-unsafe-docs") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_require_safety = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--unsafe-stats") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_stats_enabled = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--lint-inline-c-unsafe") == 0) {
            g_lint_inline_c_unsafe = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-kinds") == 0) {
            /* Phase HKT-P6: enable kind-annotation dump after kind-check */
            g_dump_kinds = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--strict-refine") == 0) {
            /* RT3: upgrade every undecided/refuted refinement obligation from
             * "keep the runtime check" to a hard compile error.  A
             * diagnostic-strictness knob, NOT an experiment -- it changes how
             * loudly the compiler reports, not what it compiles. */
            g_strict_refine = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--strict-effects") == 0) {
            /* ER1: enforce unannotated effectful functions as warnings */
            g_strict_effects = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-reflect") == 0) {
            /* reflected-measures RF5: print the totality verdict for each
             * `^reflect` site */
            g_dump_reflect = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-write-frames") == 0) {
            /* G1: print the checked verdict for each declared `#writes` frame */
            g_dump_write_frames = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-read-frames") == 0) {
            /* R4 slice 2: print the verification verdict for each `#reads` frame */
            g_dump_read_frames = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-effects") == 0) {
            /* ER6: print inferred effect row for each defn after inference */
            g_dump_effects = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-refine=json") == 0) {
            /* SX8a: one JSON record per refinement obligation, on stdout.
             * Stripped here rather than in a per-command loop so it works
             * with `check` and `emit-c` alike -- the records come from the
             * elaboration both share, and forcing a codegen run just to read
             * a report would be the wrong shape. */
            g_dump_refine_json = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps-coloring") == 0) {
            /* CPS1: print the whole-program may-capture coloring per defn */
            g_dump_cps_coloring = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps") == 0) {
            /* CPS2: print the ANF/CPS IR for each colored defn */
            g_dump_cps = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-mono-specs") == 0) {
            /* VBM1 (van-laarhoven-monomorphization-plan): print the by-value HKT
             * monomorphization spec registry after elaboration. */
            g_dump_mono_specs = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps-mono") == 0) {
            /* G1 (cps-backend-generic-monomorph-classification-plan): report CPS-
             * subset admissibility of each colored-generic monomorph. */
            g_dump_cps_mono = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-sizes") == 0) {
            /* SZ8: print inferred size index per sized-GADT constructor */
            g_dump_sizes = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--lint-effects") == 0) {
            /* Retired: it was a byte-identical copy of --strict-effects
             * (docs/archive/strict-effects-and-lint-effects-are-
             * indistinguishable.md).  Still accepted, as an alias, so
             * existing scripts keep working; TUR-W0050 says so. */
            fprintf(stderr, "warning [TUR-W0050]: --lint-effects is deprecated; "
                            "use --strict-effects, which emits the same "
                            "TUR-W0030 warnings (-Werror=strict-effects makes "
                            "them errors)\n");
            g_strict_effects = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--backtrack-depth=", 18) == 0) {
            /* Phase B5: cap run-backtrack at N results (0 = unlimited) */
            g_backtrack_depth = (int64_t)atoll(argv[i] + 18);
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--backtrack-depth") == 0 && i + 1 < argc) {
            g_backtrack_depth = (int64_t)atoll(argv[i + 1]);
            for (int j = i; j < argc - 2; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        } else if (strcmp(argv[i], "--dump-clone-plan") == 0) {
            /* Phase B5: dump cloneable capture plan after CPS */
            g_dump_clone_plan = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps-coloring") == 0) {
            /* CPS1: dump CPS coloring (colored/uncolored) after cps_transform */
            g_dump_cps_coloring = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--cps-path") == 0) {
            /* CPS3: emit CPS wrappers for colored functions */
            g_cps_path = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--emit-abi-trace") == 0) {
            /* Phase I: trace the resolved ABI path for each call site during emit */
            g_emit_abi_trace = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (is_known_deprecated_x_flag(argv[i])) {
            /* drop-x-flags-plan: every -X<name> is an accept-and-warn no-op
             * as of v0.24.0.  TUR-W0050 emitted; the underlying feature is
             * always on (see src/runtime/globals.c). */
            warn_deprecated_x_flag(argv[i]);
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--keep-contracts") == 0) {
            /* CT3: keep contract checks in release builds */
            g_keep_contracts_in_release = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--enable=", 9) == 0) {
            /* XF1: opt in to one or more experimental features (comma list).
             * An unknown name is a hard TUR-E0310 error. */
            if (!enable_experiment_list(argv[i] + 9, XF_SRC_CLI)) return 2;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--macro-caps=", 13) == 0) {
            /* Stage 3 (macro-system-direction-plan): grant the macro-time
             * env extra capabilities.  Only `io` exists; anything else --
             * including a plea for ffi/unsafe -- is a hard error, because
             * those are never offered at macro time. */
            if (strcmp(argv[i] + 13, "io") != 0) {
                fprintf(stderr,
                        "error: --macro-caps only accepts 'io' "
                        "(got '%s'); ffi/unsafe/inline-c/async are never "
                        "available at macro-expansion time\n",
                        argv[i] + 13);
                return 2;
            }
            g_macro_caps_io = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--no-proc-macros") == 0) {
            /* security-audit-plan WP3: no defmacro* body runs. */
            g_no_proc_macros = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--allow-experimental") == 0) {
            /* UC-4 (user-config-experiments-plan): --allow-experimental was
             * retired.  Enabling an experiment (via --enable=<name>,
             * build.tur, or ~/.config/turmeric/experiments.tur) is now itself
             * the acknowledgment; the separate gate added nothing.  Reject it
             * with a targeted message for one release rather than silently
             * accepting or emitting a generic "unknown flag". */
            fprintf(stderr,
                    "error: --allow-experimental was retired in v" TUR_VERSION
                    "; enabling an experiment (via --enable=<name>, build.tur, "
                    "or ~/.config/turmeric/experiments.tur) is now the "
                    "acknowledgment. Remove the flag.\n");
            return 2;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            /* no-color-flag-rejected-everywhere: already parsed into
             * `no_color`; it was never stripped, so a trailing one reached
             * the command's parser as an unknown flag and a leading one was
             * taken for the command name. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        }
    }
    /* Put the program's words back (see dd_at above). */
    if (dd_tail > 0 && argc != dd_at)
        memmove(&argv[argc], &argv[dd_at], (size_t)dd_tail * sizeof(argv[0]));
    argc += dd_tail;
    argv[argc] = NULL;
    
        /* Initialize diagnostics - use color unless --no-color or --json-diagnostics specified */
        /* JSON output disables color */
        if (use_json_output) use_json_diagnostics = true;
        diag_init(!no_color && !use_json_diagnostics && stderr_is_tty());
        diag_set_json_output(use_json_diagnostics);
    
    if (explain_mode) {
        if (!explain_code) {
            fprintf(stderr, "tur: --explain requires a code snippet argument\n");
            return usage();
        }
        return cmd_explain(explain_code);
    }
    
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
        usage();
        return 0;
    }

    if (argc < 2) return usage();
    const char *cmd = argv[1];

    {
        const char *resolved = resolve_command(cmd);
        if (resolved == COMMAND_AMBIGUOUS) return usage();
        if (resolved != NULL) cmd = resolved;
    }

    if (strcmp(cmd, "emit-c") == 0) {
        /* SC2: collect -I flags up front so both emit-c forms see them. */
        char **emit_inc = NULL;
        int    n_emit_inc = drv_parse_include_flags(argc, argv, 2, &emit_inc);
        if (n_emit_inc < 0) { free(emit_inc); return drv_usage_error(usage_build); }
        if (has_help_flag(argc, argv)) { free(emit_inc); return usage_build(); }

        /* tur emit-c [-I <dir>...] [--output-dir <dir> | --build-dir <dir> | -B <dir>]
         *            <file1> [<file2> ...]
         * build-output-directory-plan: --build-dir / -B are the new spellings;
         * --output-dir stays as a deprecated alias. */
        int od_idx = -1;
        for (int i = 2; i < argc; i++) {
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--output-dir") == 0 ||
                strcmp(argv[i], "--build-dir")  == 0 ||
                strcmp(argv[i], "-B")            == 0) { od_idx = i; break; }
        }
        if (od_idx >= 0) {
            if (od_idx + 1 >= argc) { free(emit_inc); return drv_usage_error(usage_build); }
            const char *out_dir = argv[od_idx + 1];
            /* Inputs are every non-flag arg that isn't --output-dir or its
             * value or a -I value.  Build a clean inputs list. */
            char **inputs = (char **)malloc((size_t)argc * sizeof(char *));
            int    n_inputs = 0;
            for (int i = 2; i < argc; i++) {
                int c;
                if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
                if (i == od_idx) { i++; continue; }   /* skip --output-dir and its value */
                if (strcmp(argv[i], "--no-abi-cache") == 0) continue; /* J6: global, skip */
                if (strcmp(argv[i], "--no-r7rs-gc") == 0) continue;   /* r7rs-gc: global, skip */
                if (strcmp(argv[i], "--no-saffron-gc") == 0) continue;
                if (argv[i][0] == '-') { free(inputs); free(emit_inc); return drv_usage_error(usage_build); }
                inputs[n_inputs++] = argv[i];
            }
            /* SC4+SC5: auto-discover spice src + cross-spice deps for the
             * FIRST input only.  For the multi-file --output-dir form,
             * that input typically lives inside the same spice as the
             * others; if it doesn't, an explicit -I is the right escape
             * hatch. */
            char **md_owned = NULL;
            int    n_md_owned = 0;
            Ls2ResolverCtx md_ls2 = {0};
            if (n_inputs > 0) {
                auto_append_spice_includes(inputs[0], &emit_inc, &n_emit_inc,
                                           &md_owned, &n_md_owned, &md_ls2);
            }
            ls2_resolver_ctx_set(&md_ls2);
            int rc = cmd_emit_c_to_dir(out_dir, inputs, n_inputs,
                                       (const char **)emit_inc, n_emit_inc);
            ls2_resolver_ctx_set(NULL);
            ls2_resolver_ctx_dispose(&md_ls2);
            for (int i = 0; i < n_md_owned; i++) free(md_owned[i]);
            free(md_owned);
            free(inputs);
            free(emit_inc);
            return rc;
        }

        /* tur emit-c [-I <dir>...] <file>   single file to stdout (legacy) */
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (argv[i][0] != '-') {
                if (input) { free(emit_inc); return drv_usage_error(usage_build); }
                input = argv[i];
                continue;
            }
            free(emit_inc); return drv_usage_error(usage_build);
        }
        if (!input) { free(emit_inc); return drv_usage_error(usage_build); }
        char **ec_owned = NULL; int n_ec_owned = 0;
        Ls2ResolverCtx ec_ls2 = {0};
        auto_append_spice_includes(input, &emit_inc, &n_emit_inc,
                                   &ec_owned, &n_ec_owned, &ec_ls2);
        ls2_resolver_ctx_set(&ec_ls2);
        int rc = cmd_emit_c(input, (const char **)emit_inc, n_emit_inc);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&ec_ls2);
        for (int i = 0; i < n_ec_owned; i++) free(ec_owned[i]);
        free(ec_owned);
        free(emit_inc);
        return rc;
    }
    if (strcmp(cmd, "emit-h") == 0) {
        /* SC2: accept -I just like emit-c / check / build.
         * SC4+SC5: auto-discover enclosing spice src/ and dep src/. */
        char **eh_inc = NULL;
        int    n_eh_inc = drv_parse_include_flags(argc, argv, 2, &eh_inc);
        if (n_eh_inc < 0) { free(eh_inc); return drv_usage_error(usage_build); }
        if (has_help_flag(argc, argv)) { free(eh_inc); return usage_build(); }
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (argv[i][0] != '-') {
                if (input) { free(eh_inc); return drv_usage_error(usage_build); }
                input = argv[i];
                continue;
            }
            free(eh_inc); return drv_usage_error(usage_build);
        }
        if (!input) { free(eh_inc); return drv_usage_error(usage_build); }
        char **eh_owned = NULL; int n_eh_owned = 0;
        Ls2ResolverCtx eh_ls2 = {0};
        auto_append_spice_includes(input, &eh_inc, &n_eh_inc,
                                   &eh_owned, &n_eh_owned, &eh_ls2);
        ls2_resolver_ctx_set(&eh_ls2);
        int rc = cmd_emit_h(input, (const char **)eh_inc, n_eh_inc);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&eh_ls2);
        for (int i = 0; i < n_eh_owned; i++) free(eh_owned[i]);
        free(eh_owned);
        free(eh_inc);
        return rc;
    }
    if (strcmp(cmd, "check") == 0) {
        /* Phase 8: tur check subcommand - type-check only, no codegen
         * SC1: now accepts -I <dir> flags so per-file checks inside a spice
         * resolve their intra-spice imports the same way `tur build` does.
         * SC4: walks up to find an enclosing build.tur and auto-adds its
         * `src/` so editors / format-on-save don't need explicit -I. */
        char       **check_inc = NULL;
        int          n_check_inc = drv_parse_include_flags(argc, argv, 2, &check_inc);
        if (n_check_inc < 0) { free(check_inc); return drv_usage_error(usage_check); }
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(check_inc); return usage_check();
            }
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--no-auto-spice") == 0) continue;
            if (strcmp(argv[i], "--no-auto-stdlib") == 0) {
                g_no_auto_stdlib = true;
                continue;
            }
            if (argv[i][0] != '-') {
                if (input) { free(check_inc); return drv_usage_error(usage_check); }
                input = argv[i];
                continue;
            }
            free(check_inc); return drv_usage_error(usage_check);
        }
        if (!input) { free(check_inc); return drv_usage_error(usage_check); }
        /* A directory argument checks every .tur file under it (project /
         * spice mode), mirroring `tur test <dir>`. */
        if (drv_is_directory(input)) {
            free(check_inc);
            return cmd_check_dir(input);
        }
        char **ck_owned = NULL; int n_ck_owned = 0;
        Ls2ResolverCtx ck_ls2 = {0};
        auto_append_spice_includes(input, &check_inc, &n_check_inc,
                                   &ck_owned, &n_ck_owned, &ck_ls2);
        ls2_resolver_ctx_set(&ck_ls2);
        int rc;
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(input, &rm_n);
        if (use_json_output) {
            diag_lsp_begin();
            Buf out;
            buf_init(&out);
            compile_to_c(input, &out, (const char **)check_inc, n_check_inc,
                         (const char **)rm_p, rm_n);
            buf_free(&out);
            diag_lsp_flush(stdout);
            diag_lsp_end();
            rc = diag_had_error() ? 1 : 0;
        } else {
            Buf out;
            buf_init(&out);
            rc = compile_to_c(input, &out, (const char **)check_inc, n_check_inc,
                              (const char **)rm_p, rm_n);
            buf_free(&out);
        }
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&ck_ls2);
        free_reader_macro_paths(rm_p, rm_n);
        for (int i = 0; i < n_ck_owned; i++) free(ck_owned[i]);
        free(ck_owned);
        free(check_inc);
        return rc;
    }
    if (strcmp(cmd, "expand") == 0) {
        /* `tur expand`: type-check like `tur check`, but with the expansion
         * trace on -- every macro expansion outside the stdlib preload prints
         * to stdout (see the g_dump_expansion site in elab_call.c).  Same
         * include-path plumbing as `tur check`. */
        char       **exp_inc = NULL;
        int          n_exp_inc = drv_parse_include_flags(argc, argv, 2, &exp_inc);
        if (n_exp_inc < 0) { free(exp_inc); return drv_usage_error(usage_expand); }
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(exp_inc); return usage_expand();
            }
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--no-auto-spice") == 0) continue;
            if (strcmp(argv[i], "--no-auto-stdlib") == 0) {
                g_no_auto_stdlib = true;
                continue;
            }
            if (argv[i][0] != '-') {
                if (input) { free(exp_inc); return drv_usage_error(usage_expand); }
                input = argv[i];
                continue;
            }
            free(exp_inc); return drv_usage_error(usage_expand);
        }
        if (!input) { free(exp_inc); return drv_usage_error(usage_expand); }
        char **exp_owned = NULL; int n_exp_owned = 0;
        Ls2ResolverCtx exp_ls2 = {0};
        auto_append_spice_includes(input, &exp_inc, &n_exp_inc,
                                   &exp_owned, &n_exp_owned, &exp_ls2);
        ls2_resolver_ctx_set(&exp_ls2);
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(input, &rm_n);
        g_dump_expansion = true;
        Buf out;
        buf_init(&out);
        int rc = compile_to_c(input, &out, (const char **)exp_inc, n_exp_inc,
                              (const char **)rm_p, rm_n);
        buf_free(&out);
        g_dump_expansion = false;
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&exp_ls2);
        free_reader_macro_paths(rm_p, rm_n);
        for (int i = 0; i < n_exp_owned; i++) free(exp_owned[i]);
        free(exp_owned);
        free(exp_inc);
        return rc;
    }
    if (strcmp(cmd, "audit-spans") == 0) {
        /* debugger Phase 1: elaborate <file> and report breakpoint-eligible
         * AST nodes (top-level forms, defns, let forms, call sites) that lack
         * a usable source span.  Auto-discovers the enclosing spice src/ and
         * -I flags exactly like `tur check`, so intra-spice imports resolve.
         * Exit codes: 0 = clean, 3 = holes found, 1 = could not elaborate,
         * 2 = file error. */
        char       **as_inc = NULL;
        int          n_as_inc = drv_parse_include_flags(argc, argv, 2, &as_inc);
        if (n_as_inc < 0) { free(as_inc); return drv_usage_error(usage_check); }
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(as_inc); return usage_check();
            }
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--no-auto-spice") == 0) continue;
            if (strcmp(argv[i], "--no-auto-stdlib") == 0) {
                g_no_auto_stdlib = true;
                continue;
            }
            if (argv[i][0] != '-') {
                if (input) { free(as_inc); return drv_usage_error(usage_check); }
                input = argv[i];
                continue;
            }
            free(as_inc); return drv_usage_error(usage_check);
        }
        if (!input) { free(as_inc); return drv_usage_error(usage_check); }
        char **as_owned = NULL; int n_as_owned = 0;
        Ls2ResolverCtx as_ls2 = {0};
        auto_append_spice_includes(input, &as_inc, &n_as_inc,
                                   &as_owned, &n_as_owned, &as_ls2);
        ls2_resolver_ctx_set(&as_ls2);
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(input, &rm_n);
        Buf out;
        buf_init(&out);
        g_audit_spans = true;
        g_audit_span_holes = 0;
        int rc = compile_to_c(input, &out, (const char **)as_inc, n_as_inc,
                              (const char **)rm_p, rm_n);
        g_audit_spans = false;
        buf_free(&out);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&as_ls2);
        free_reader_macro_paths(rm_p, rm_n);
        for (int i = 0; i < n_as_owned; i++) free(as_owned[i]);
        free(as_owned);
        free(as_inc);
        return rc;
    }
    if (strcmp(cmd, "lsp") == 0) {
        diag_init(false);   /* no color -- stdout is reserved for JSON-RPC */
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--rename-exports") == 0)
                lsp_set_rename_exports(true);
        }
        lsp_server_run(STDIN_FILENO, STDOUT_FILENO);
        return 0;
    }
    if (strcmp(cmd, "mcp") == 0) {
        diag_init(false);   /* no color -- stdout is reserved for JSON-RPC */
        mcp_server_run(STDIN_FILENO, STDOUT_FILENO);
        return 0;
    }
    /* Debugger Phase 3: DAP server over the interpreter (JSON-RPC / stdio). */
    if (strcmp(cmd, "dap") == 0) {
        return cmd_dap();
    }
    /* Track T1: record an interpreted run, or read a recording back. */
    if (strcmp(cmd, "trace") == 0) {
        const char *file = NULL, *out_path = NULL;
        uint32_t    max_steps = 0;
        bool        dump = false;
        TurTraceGrain grain = TUR_TRACE_GRAIN_NODE;
        char      **prog_argv = NULL;
        int         prog_argc = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--dump") == 0) { dump = true; continue; }
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out_path = argv[++i];
                continue;
            }
            if (strncmp(argv[i], "--max-steps=", 12) == 0) {
                max_steps = (uint32_t)strtoul(argv[i] + 12, NULL, 10);
                continue;
            }
            /* The old granularity, for a program too big to record per
             * expression under the cap.  Not the default: see TurTraceGrain. */
            if (strcmp(argv[i], "--lines") == 0) {
                grain = TUR_TRACE_GRAIN_LINE; continue;
            }
            if (!file) { file = argv[i]; continue; }
            /* Everything after the program is the program's own argv. */
            prog_argv = &argv[i];
            prog_argc = argc - i;
            break;
        }
        if (!file) {
            fprintf(stderr,
                "usage: tur trace <file.tur> [-o out.turtrace] "
                "[--max-steps=N] [--lines] [-- args...]\n"
                "       tur trace --dump <file.turtrace>\n"
                "\n"
                "  A step is one expression.  --lines records one step per\n"
                "  source line instead, which is coarser: nested calls on one\n"
                "  line collapse into a single step.\n");
            return 1;
        }
        if (dump) return cmd_trace_dump(file);
        return cmd_trace(file, out_path, prog_argv, prog_argc, max_steps, grain);
    }
    /* lsp-lite: completion/calltip/doc backend for lightweight editors.
     * Newline-delimited JSON over stdio; stdout is reserved for protocol
     * traffic. */
    if (strcmp(cmd, "lsp-lite") == 0) {
        diag_init(false);
        resolve_stdlib_root();   /* sets TUR_STDLIB_DIR so lsp_lite can find docstrings.tur */
        return lsp_lite_run(STDIN_FILENO, STDOUT_FILENO);
    }
    if (strcmp(cmd, "build") == 0) {
        const char *input = NULL;
        const char *out = NULL;
        const char *build_target = NULL;
        bool        shared = false;  /* RP0: --shared selects shared-library build */
        const char *manifest_out = NULL; /* RP1: --manifest <path> override */
        const char *cli_build_dir = NULL; /* build-output-directory-plan: --build-dir/-B */
        /* tur-link-and-build-split-plan Phase 3b/4: opt in to the compile+link
         * split for a single-file build.  Default stays the monolithic `cc`
         * call so output is byte-identical until the split is proven; the two
         * flags let a build force either path during rollout. */
        int split_build = 0;  /* 0 = default(monolithic), 1 = split, -1 = forced monolithic */
        /* SC1: collect -I flags once via the shared helper, then walk argv
         * a second time for the build-specific flags (`-o`, `--target`)
         * and the positional input. */
        char  **build_inc = NULL;
        int     n_build_inc = drv_parse_include_flags(argc, argv, 2, &build_inc);
        if (n_build_inc < 0) { free(build_inc); return drv_usage_error(usage_build); }
        for (int i = 2; i < argc; i++) {
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(build_inc); return usage_build();
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (strcmp(argv[i], "--shared") == 0) {
                shared = true;
            } else if (strcmp(argv[i], "--bundle-deps") == 0) {
                g_build_bundle_deps = true;
            } else if (strcmp(argv[i], "--release") == 0) {
                g_cc_release = true;
            } else if (strcmp(argv[i], "--split-build") == 0) {
                split_build = 1;
            } else if (strcmp(argv[i], "--no-split-build") == 0) {
                split_build = -1;
            } else if (strncmp(argv[i], "--runtime=", 10) == 0 ||
                       (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc)) {
                const char *mode = argv[i][9] == '=' ? argv[i] + 10 : argv[++i];
                if (strcmp(mode, "lib") == 0) g_runtime_mode = TUR_RT_LIB;
                else if (strcmp(mode, "source") == 0) g_runtime_mode = TUR_RT_SOURCE;
                else if (strcmp(mode, "auto") == 0) g_runtime_mode = TUR_RT_AUTO;
                else if (strcmp(mode, "split") == 0) g_runtime_mode = TUR_RT_SPLIT;
                else {
                    fprintf(stderr, "tur build: unknown --runtime '%s' "
                            "(supported: auto, lib, source, split)\n", mode);
                    free(build_inc); return 1;
                }
            } else if (strcmp(argv[i], "--no-abi-cache") == 0) {
                /* J6: consumed globally by parse_no_abi_cache; no-op here. */
            } else if (strcmp(argv[i], "--no-r7rs-gc") == 0 ||
                       strcmp(argv[i], "--no-saffron-gc") == 0) {
                /* r7rs-gc: consumed globally by parse_no_r7rs_gc /
                 * parse_no_saffron_gc; no-op here. */
            } else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
                manifest_out = argv[++i];
            } else if ((strcmp(argv[i], "--build-dir") == 0 ||
                        strcmp(argv[i], "-B") == 0) && i + 1 < argc) {
                cli_build_dir = argv[++i];
            } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
                build_target = argv[++i];
                if (strcmp(build_target, "wasm") != 0) {
                    fprintf(stderr, "tur build: unknown target '%s' (supported: wasm)\n", build_target);
                    free(build_inc); return 1;
                }
            } else if (argv[i][0] != '-') {
                if (input) { free(build_inc); return drv_usage_error(usage_build); }
                input = argv[i];
            } else {
                free(build_inc); return drv_usage_error(usage_build);
            }
        }
        if (!input) { free(build_inc); return drv_usage_error(usage_build); }
        /* RP0: --shared only supports directory input -- the single-file
         * build path emits a `static`-by-default function and a main(), so
         * dlsym wouldn't find anything useful. Directory mode runs through
         * compile_to_implementation in separate-compilation mode, which
         * gives exported defns extern linkage. */
        if (shared && !drv_is_directory(input)) {
            fprintf(stderr, "tur build: --shared requires a directory argument "
                            "(single-file builds emit static symbols)\n");
            free(build_inc); return 1;
        }
        if (shared && build_target) {
            fprintf(stderr, "tur build: --shared and --target are mutually exclusive\n");
            free(build_inc); return 1;
        }
        if (manifest_out && !shared) {
            fprintf(stderr, "tur build: --manifest requires --shared\n");
            free(build_inc); return 1;
        }
        if (g_build_bundle_deps && !shared) {
            fprintf(stderr, "tur build: --bundle-deps requires --shared\n");
            free(build_inc); return 1;
        }
        /* Check if input is a directory - use multi-file build */
        int rc;
        if (drv_is_directory(input)) {
            /* Manifest-driven project build when the directory carries a
             * build.tur: read the manifest, descend into src/, and resolve
             * the include path (own src/ + each :spices dep). Otherwise fall
             * back to the bare-directory glob. */
            char proj_manifest[4096];
            snprintf(proj_manifest, sizeof(proj_manifest),
                     "%s/build.tur", input);
            struct stat mst;
            if (stat(proj_manifest, &mst) == 0 && S_ISREG(mst.st_mode)) {
                rc = cmd_build_project(input, out, shared, manifest_out,
                                       (const char **)build_inc, n_build_inc,
                                       cli_build_dir, build_target);
            } else {
                rc = cmd_build_multi(input, out, shared, manifest_out,
                                     cli_build_dir);
            }
        } else {
            /* RM4: auto-discover the spice manifest containing `input` so
             * `:reader-macros [...]` entries apply when building a single
             * file from inside a project. No-op when there is no manifest.
             * The shared helper also applies the manifest's `:experiments`
             * (and the user experiments file): a hand-rolled copy of this
             * walk skipped them, so `tur build f.tur` compiled with a
             * project's gates off where `tur check f.tur` had them on. */
            char **b_rm  = NULL;
            int    b_n   = 0;
            b_rm = discover_manifest_reader_macros(input, &b_n);
            /* Mirror emit-c / emit-h / check / run: widen the include
             * path with the enclosing spice's src/ and every declared
             * `:spices` dep's src/. Short-circuits when --no-auto-spice
             * is set or there is no enclosing build.tur. */
            char **b_owned = NULL; int n_b_owned = 0;
            Ls2ResolverCtx b_ls2 = {0};
            auto_append_spice_includes(input, &build_inc, &n_build_inc,
                                       &b_owned, &n_b_owned, &b_ls2);
            ls2_resolver_ctx_set(&b_ls2);
            if (split_build == 1 && !build_target) {
                /* tur-link-and-build-split-plan Phase 3b: `tur build` defined as
                 * `tur compile` + `tur link` composed -- the exact subcommand
                 * code paths, so the three can never drift.  The generated
                 * object + `.link` sidecar land under the build temp dir. */
                char base[512];
                drv_default_output_name(input, base, sizeof(base));
                char objp[1200];
                snprintf(objp, sizeof(objp), "%s%s.o", drv_stable_c_prefix(), base);
                rc = cmd_compile(input, objp, (const char **)build_inc,
                                 n_build_inc, (const char **)b_rm, b_n);
                if (rc == 0) {
                    const char *outp = out;
                    char outbuf[1024];
                    if (!outp) {
                        drv_default_output_name(input, outbuf, sizeof(outbuf));
                        outp = outbuf;
                    }
                    const char *lk[1] = { objp };
                    rc = cmd_link(outp, lk, 1, false, NULL);
                }
                /* Drop the intermediate .o/.c/.link (ccache keyed on content
                 * already caches the underlying object compile). */
                char scrap[1300];
                unlink(objp);
                drv_obj_sibling_path(objp, ".c",    scrap, sizeof(scrap)); unlink(scrap);
                drv_obj_sibling_path(objp, ".link", scrap, sizeof(scrap)); unlink(scrap);
            } else {
                rc = cmd_build(input, out, (const char **)build_inc, n_build_inc,
                               build_target, (const char **)b_rm, b_n);
            }
            ls2_resolver_ctx_set(NULL);
            ls2_resolver_ctx_dispose(&b_ls2);
            for (int i = 0; i < n_b_owned; i++) free(b_owned[i]);
            free(b_owned);
            free_reader_macro_paths(b_rm, b_n);
        }
        free(build_inc);
        return rc;
    }
    /* tur-link-and-build-split-plan Phase 3a-0: tur compile <in.tur> -o <out.o> */
    if (strcmp(cmd, "compile") == 0) {
        const char *input = NULL;
        const char *out = NULL;
        const char *cli_build_dir = NULL;
        char  **comp_inc = NULL;
        int     n_comp_inc = drv_parse_include_flags(argc, argv, 2, &comp_inc);
        if (n_comp_inc < 0) { free(comp_inc); return drv_usage_error(usage_build); }
        for (int i = 2; i < argc; i++) {
            int c;
            if (drv_is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(comp_inc); return usage_build();
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if ((strcmp(argv[i], "--build-dir") == 0 ||
                        strcmp(argv[i], "-B") == 0) && i + 1 < argc) {
                cli_build_dir = argv[++i];
            } else if (strncmp(argv[i], "--runtime=", 10) == 0 ||
                       (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc)) {
                const char *mode = argv[i][9] == '=' ? argv[i] + 10 : argv[++i];
                if (strcmp(mode, "lib") == 0) g_runtime_mode = TUR_RT_LIB;
                else if (strcmp(mode, "source") == 0) g_runtime_mode = TUR_RT_SOURCE;
                else if (strcmp(mode, "auto") == 0) g_runtime_mode = TUR_RT_AUTO;
                else {
                    fprintf(stderr, "tur compile: unknown --runtime '%s' "
                            "(supported: auto, lib, source)\n", mode);
                    free(comp_inc); return 1;
                }
            } else if (strcmp(argv[i], "--no-abi-cache") == 0 ||
                       strcmp(argv[i], "--no-r7rs-gc") == 0 ||
                       strcmp(argv[i], "--no-saffron-gc") == 0) {
                /* global, consumed elsewhere */
            } else if (argv[i][0] != '-') {
                if (input) { free(comp_inc); return drv_usage_error(usage_build); }
                input = argv[i];
            } else {
                free(comp_inc); return drv_usage_error(usage_build);
            }
        }
        if (!input) { free(comp_inc); return drv_usage_error(usage_build); }
        if (drv_is_directory(input)) {
            fprintf(stderr, "tur compile: expects a single .tur file, not a directory\n");
            free(comp_inc); return 1;
        }
        /* Default the object under <build-dir>/obj/ when --build-dir/-B (or a
         * manifest :build-dir) applies and no explicit -o was given. */
        char obj_default[1200];
        if (!out) {
            char *bd = drv_resolve_build_dir(input, cli_build_dir);
            if (bd) {
                char base[512];
                drv_default_output_name(input, base, sizeof(base));
                snprintf(obj_default, sizeof(obj_default), "%s/obj/%s.o", bd, base);
                out = obj_default;
                free(bd);
            }
        }
        /* Reader macros, experiments + spice include auto-discovery,
         * mirroring cmd_build. */
        char **c_rm = NULL; int c_n = 0;
        c_rm = discover_manifest_reader_macros(input, &c_n);
        char **c_owned = NULL; int n_c_owned = 0;
        Ls2ResolverCtx c_ls2 = {0};
        auto_append_spice_includes(input, &comp_inc, &n_comp_inc,
                                   &c_owned, &n_c_owned, &c_ls2);
        ls2_resolver_ctx_set(&c_ls2);
        int rc = cmd_compile(input, out, (const char **)comp_inc, n_comp_inc,
                             (const char **)c_rm, c_n);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&c_ls2);
        for (int i = 0; i < n_c_owned; i++) free(c_owned[i]);
        free(c_owned);
        free_reader_macro_paths(c_rm, c_n);
        free(comp_inc);
        return rc;
    }
    /* tur-link-and-build-split-plan Phase 3a: tur link <obj/src>... -o <out> */
    if (strcmp(cmd, "link") == 0) {
        const char *out = NULL;
        bool shared = false;
        const char *link_flags = NULL;
        const char **inputs = (const char **)malloc((size_t)argc * sizeof(char *));
        int n_inputs = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(inputs); return usage_build();
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (strcmp(argv[i], "--shared") == 0) {
                shared = true;
            } else if (strcmp(argv[i], "--link-flags") == 0 && i + 1 < argc) {
                link_flags = argv[++i];
            } else if (argv[i][0] != '-') {
                inputs[n_inputs++] = argv[i];
            } else {
                fprintf(stderr, "tur link: unknown option '%s'\n", argv[i]);
                free(inputs); return drv_usage_error(usage_build);
            }
        }
        if (n_inputs == 0) { free(inputs); return drv_usage_error(usage_build); }
        int rc = cmd_link(out, inputs, n_inputs, shared, link_flags);
        free(inputs);
        return rc;
    }
    if (strcmp(cmd, "jit") == 0) {
        return cmd_jit(argc, argv);
    }
    if (strcmp(cmd, "emit-rt-split") == 0) {
        return cmd_emit_rt_split(argc, argv);
    }
    if (strcmp(cmd, "run") == 0) {
        /* Disambiguate: if the first non-flag argument ends in .tur or
         * .tur.sweet, use the classic compile-and-run path; if --release /
         * -I flags appear (compile-only flags), also use classic path.
         * Otherwise dispatch to the Justfile task runner (RN0-RN7). */
        bool use_classic = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--") == 0) break;
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                /* Default help to the task runner; it documents both modes. */
                return usage_justrun();
            }
            if (strcmp(argv[i], "--release") == 0 ||
                strcmp(argv[i], "--offline") == 0  ||
                strcmp(argv[i], "--engine") == 0   ||
                strncmp(argv[i], "--engine=", 9) == 0 ||
                strncmp(argv[i], "-I", 2) == 0) {
                /* --engine is a classic-path (compile/run) flag; without this
                 * arm its VALUE would be taken as a Justfile task name. */
                use_classic = true;
                break;
            }
            if (argv[i][0] != '-' || strcmp(argv[i], "-") == 0) {
                /* Check if it looks like a .tur file path */
                const char *a = argv[i];
                size_t an = strlen(a);
                if ((an > 4  && strcmp(a + an - 4,  ".tur")      == 0) ||
                    (an > 10 && strcmp(a + an - 10, ".tur.sweet") == 0) ||
                    (an > 4  && strcmp(a + an - 4,  ".scm")      == 0) ||
                    strcmp(a, "-") == 0) {
                    use_classic = true;
                }
                break;
            }
        }
        if (use_classic)
            return cmd_run(argc, argv);
        /* engine-selection-plan E3: a BARE `tur run` (no recipe, no file) in
         * a Justfile-less build.tur project takes the classic project-run
         * path -- which resolves the manifest's `:engine` -- instead of the
         * historical hard 127.  A named recipe keeps the task-runner error:
         * a typo'd task silently compiling-and-running would be worse. */
        {
            bool bare = true;
            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "--") == 0) break;
                if (argv[i][0] != '-') { bare = false; break; }
                if (strcmp(argv[i], "--justfile") == 0 ||
                    strcmp(argv[i], "--chdir") == 0) i++;   /* skip value */
            }
            if (bare && !justrun_finds_justfile()) {
                char _cwd[4096];
                if (getcwd(_cwd, sizeof(_cwd))) {
                    char *_root = drv_find_project_root(_cwd);
                    if (_root) {
                        free(_root);
                        return cmd_run(argc, argv);
                    }
                }
            }
        }
        return cmd_justrun(argc, argv);
    }
    if (strcmp(cmd, "repl") == 0) {
        /* Phase S0: interactive REPL.
         * RP6: --watch enables auto-reload between prompts when a
         * spice source file changes mtime. No background thread --
         * the freshness check runs synchronously each turn. */
        bool watch_mode = false;
        const char *repl_engine_flag = NULL;
        /* saffron-lang-plan S8: start the session in a dialect, rather than
         * making the user type `#lang saffron` as their first line.  Both
         * routes land on the same env switch. */
        const char *repl_lang_flag = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                return usage_repl();
            }
            if (strcmp(argv[i], "--watch") == 0) {
                watch_mode = true;
                continue;
            }
            if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
                repl_lang_flag = argv[++i];
                continue;
            }
            if (strncmp(argv[i], "--lang=", 7) == 0) {
                repl_lang_flag = argv[i] + 7;
                continue;
            }
            if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc) {
                repl_engine_flag = argv[++i];
                continue;
            }
            if (strncmp(argv[i], "--engine=", 9) == 0) {
                repl_engine_flag = argv[i] + 9;
                continue;
            }
            fprintf(stderr, "tur repl: unknown option '%s'\n", argv[i]);
            return drv_usage_error(usage_repl);
        }
        /* J2 (jit-engine-plan 3.3): spice auto-discovery can build in process
         * through the MIR engine instead of the `tur build --shared`
         * subprocess + dlopen.  This used to hang off `--enable=jit`; that
         * experiment graduated 2026-08-17, so it hangs off ENGINE SELECTION
         * now -- `--engine jit`, `TUR_ENGINE=jit`, or `:engine "jit"` in the
         * enclosing build.tur, the same ladder `tur run` resolves.
         *
         * Graduating the experiment deliberately did NOT make this the
         * default: unset, the engine resolves to "cc" and the subprocess path
         * is byte-for-byte what it was. */
        {
            const char *eng = drv_resolve_engine(".", repl_engine_flag);
            if (!eng) return 2;    /* TUR-E0311 already printed */
            if (strcmp(eng, "jit") == 0) {
#ifdef TUR_HAVE_JIT
                tur_spice_set_jit_hook(&g_repl_jit_hook);
#else
                fprintf(stderr,
                        "tur repl: engine \"jit\" is configured, but this "
                        "build carries no JIT engine\n"
                        "     reconfigure with -DTUR_JIT=ON (the default on "
                        "64-bit x86-64/arm64),\n"
                        "     or override the engine: --engine cc / "
                        "TUR_ENGINE=cc\n");
                return 2;
#endif
            }
        }
        /* Validated HERE rather than inside cmd_repl, so a typo is a usage
         * error before the banner prints and the stdlib preloads. */
        if (repl_lang_flag) {
            /* r7rs-lang-plan R9: any base `tur dialects` lists, resolved
             * through the same table the `#lang` line uses -- it was a
             * hard-coded "turmeric"/"saffron" pair, so `--lang r7rs` was
             * refused. */
            LangDialect ld;
            ReaderType lr;
            if (!lang_base_lookup(repl_lang_flag, strlen(repl_lang_flag), &ld, &lr)) {
                fprintf(stderr,
                        "tur repl: unknown --lang '%s' "
                        "(expected a base `tur dialects` lists, e.g. "
                        "\"turmeric\", \"saffron\" or \"r7rs\")\n",
                        repl_lang_flag);
                return drv_usage_error(usage_repl);
            }
            g_repl_start_lang = repl_lang_flag;
        }
        return cmd_repl(watch_mode);
    }
    /* Tier 3: persistent fixture worker for the test suite. */
    if (strcmp(cmd, "worker") == 0) {
        return cmd_worker();
    }
    if (strcmp(cmd, "interpret") == 0 || strcmp(cmd, "--interpret") == 0) {
        char **inc = NULL;
        int    n_inc = 0;
        int    fi = interp_leading_include_flags(argc, argv, 2, &inc, &n_inc);
        if (fi < 0) return 2;
        if (fi >= argc) {
            free(inc);
            fprintf(stderr, "tur: %s requires a file argument\n", cmd);
            return usage();
        }
        int rc = cmd_eval_inc(argv[fi], !no_color && stderr_is_tty(),
                              inc, n_inc, argv + fi + 1, argc - fi - 1,
                              /*debug=*/false);
        free(inc);
        return rc;
    }
    /* Debugger Phase 2: `tur debug <file.tur> [args...]` -- run a file through
     * the tree-walking interpreter under the interactive debugger.  Drops into
     * a command REPL at program entry; commands are read from stdin (so a
     * script can drive it).  See docs/archive/history/debugger-plan.md (Phase 2). */
    if (strcmp(cmd, "debug") == 0) {
        if (argc < 3 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
            fprintf(stderr,
                "usage:\n  tur debug <file.tur> [args...]\n"
                "\nDrops into an interactive debugger (break/step/next/finish/\n"
                "backtrace/locals/print/list/continue). Type 'help' at the\n"
                "(tur-dbg) prompt for the full command list.\n");
            return argc < 3 ? 1 : 0;
        }
        char **inc = NULL;
        int    n_inc = 0;
        int    fi = interp_leading_include_flags(argc, argv, 2, &inc, &n_inc);
        if (fi < 0) return 2;
        if (fi >= argc) {
            free(inc);
            fprintf(stderr, "tur: debug requires a file argument\n");
            return 1;
        }
        int rc = cmd_eval_inc(argv[fi], !no_color && stderr_is_tty(),
                              inc, n_inc, argv + fi + 1, argc - fi - 1,
                              /*debug=*/true);
        free(inc);
        return rc;
    }
    /* E3: tur eval '<expr>' or tur eval --file <file> */
    if (strcmp(cmd, "eval") == 0) {
        if (argc < 3) return drv_usage_error(usage_eval);
        bool is_file = false;
        const char *src = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
                return usage_eval();
            int consumed = 0;
            if (drv_is_include_flag(argc, argv, i, &consumed)) {
                i += consumed - 1;
                continue;
            }
            if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
                is_file = true;
                src = argv[++i];
            } else if (argv[i][0] != '-') {
                src = argv[i];
            }
        }
        if (!src) return drv_usage_error(usage_eval);
        bool use_color = !no_color && stderr_is_tty();
        if (is_file) {
            char **inc = NULL;
            int    n_inc = drv_parse_include_flags(argc, argv, 2, &inc);
            if (n_inc < 0) {
                fprintf(stderr, "tur: -I requires a directory argument\n");
                return 2;
            }
            int rc = cmd_eval_inc(src, use_color, inc, n_inc, NULL, 0,
                                  /*debug=*/false);
            free(inc);
            return rc;
        }
        return cmd_eval_expr(src, use_color);
    }
    /* E5: tur doc <symbol> */
    if (strcmp(cmd, "doc") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
            return usage_doc();
        if (argc != 3) return drv_usage_error(usage_doc);
        return cmd_doc_cli(argv[2]);
    }
    /* OD4: tur docs -- locate/open/serve the rendered guides and API pages. */
    if (strcmp(cmd, "docs") == 0)
        return cmd_docs(argc, argv);
    /* AI6: tur image-info <image> -- print header without resuming. */
    if (strcmp(cmd, "image-info") == 0) {
        if (argc != 3 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
            fprintf(stderr, "usage:\n  tur image-info <image>\n");
            return argc == 3 ? 0 : 1;
        }
        return cmd_image_info(argv[2]);
    }
    /* AI6: tur image-verify <image> [binary] -- validate header (+ stamp). */
    if (strcmp(cmd, "image-verify") == 0) {
        if (argc < 3 || argc > 4 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
            fprintf(stderr, "usage:\n  tur image-verify <image> [loader-binary]\n");
            return (argc >= 3 && strcmp(argv[2], "--help") != 0) ? 2 : 0;
        }
        return cmd_image_verify(argv[2], argc == 4 ? argv[3] : NULL);
    }
    /* E13: tur explain — first-class subcommand wrapping --explain */
    if (strcmp(cmd, "explain") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
            return usage_explain();
        if (argc < 3) return drv_usage_error(usage_explain);
        return cmd_explain(argv[2]);
    }
    if (strcmp(cmd, "format") == 0) {
        bool check_only = false;
        bool diff_mode  = false;
        const char *fmt_input = NULL;
        bool        lang_set  = false;
        ReaderType  force_lang = READER_TURMERIC;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
                return usage_format();
            if (strcmp(argv[i], "--check") == 0) {
                check_only = true;
            } else if (strcmp(argv[i], "--diff") == 0) {
                diff_mode = true;
            } else if (strcmp(argv[i], "--lang") == 0) {
                if (i + 1 >= argc) return drv_usage_error(usage_format);
                const char *lang = argv[++i];
                if (!fmt_reader_from_lang_name(lang, &force_lang)) {
                    fprintf(stderr,
                            "tur format: unknown dialect '%s' (expected a "
                            "reader name or a base `tur dialects` lists)\n",
                            lang);
                    return 2;
                }
                lang_set = true;
            } else if (argv[i][0] != '-') {
                if (fmt_input) return drv_usage_error(usage_format);
                fmt_input = argv[i];
            } else {
                return drv_usage_error(usage_format);
            }
        }
        if (check_only && diff_mode) return drv_usage_error(usage_format);
        return cmd_format(fmt_input, check_only, diff_mode, lang_set,
                          force_lang);
    }
    if (strcmp(cmd, "fmt") == 0)
        return cmd_fmt(argc, argv);
    if (strcmp(cmd, "parse-check") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 ||
                          strcmp(argv[2], "-h") == 0))
            return usage_parse_check();
        if (argc != 4) return drv_usage_error(usage_parse_check);
        return cmd_parse_check(argv[2], argv[3]);
    }
    if (strcmp(cmd, "demangle") == 0)
        return cmd_demangle(argc, argv);
    if (strcmp(cmd, "test") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
            return usage_test();
        if (argc != 3) return drv_usage_error(usage_test);
        return cmd_test(argv[2]);
    }
    /* Phase PKG-1: Spice package manager commands */
    if (strcmp(cmd, "new") == 0)
        return cmd_pkg_new(argc, argv);
    if (strcmp(cmd, "init") == 0)
        return cmd_pkg_init(argc, argv);
    if (strcmp(cmd, "add") == 0)
        return cmd_pkg_add(argc, argv);
    if (strcmp(cmd, "add-cmake") == 0)
        return cmd_pkg_add_cmake(argc, argv);
    if (strcmp(cmd, "fetch") == 0)
        return cmd_pkg_fetch(argc, argv);
    if (strcmp(cmd, "audit") == 0)
        return cmd_pkg_audit(argc, argv);
    if (strcmp(cmd, "emit-cmake") == 0)
        return cmd_pkg_emit_cmake(argc, argv);
    /* GS-M2: global spice install commands */
    if (strcmp(cmd, "install") == 0)
        return cmd_pkg_install(argc, argv);
    if (strcmp(cmd, "uninstall") == 0)
        return cmd_pkg_uninstall(argc, argv);
    /* GS-M3 / GS-M4 */
    if (strcmp(cmd, "list") == 0)
        return cmd_pkg_list(argc, argv);
    if (strcmp(cmd, "upgrade") == 0)
        return cmd_pkg_upgrade(argc, argv);
    /* SX8a: run an SMT-LIB2 script through the refinement solver */
    if (strcmp(cmd, "smt") == 0)
        return cmd_smt(argc, argv);
    /* XF3: experimental-feature registry listing */
    if (strcmp(cmd, "experiments") == 0)
        return cmd_experiments(argc, argv);
    /* `#lang` base-dialect listing */
    if (strcmp(cmd, "dialects") == 0)
        return cmd_dialects(argc, argv);
    /* Shell completion scripts (zsh/bash) */
    if (strcmp(cmd, "completion") == 0)
        return cmd_completion(argc, argv);

    /* GS-M2: subcommand fallthrough — `tur foo bar` execs `tur-foo bar`
     * from $PATH when "foo" isn't a built-in. Built-ins always win.
     * If exec succeeds, this does not return. */
    return try_external_subcommand(argc, argv);
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
