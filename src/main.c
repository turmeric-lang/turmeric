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

int drv_usage(void) {
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
    g_no_color = parse_no_color(argc, argv);
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
             * `g_no_color`; it was never stripped, so a trailing one reached
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
        diag_init(!g_no_color && !use_json_diagnostics && stderr_is_tty());
        diag_set_json_output(use_json_diagnostics);
    
    if (explain_mode) {
        if (!explain_code) {
            fprintf(stderr, "tur: --explain requires a code snippet argument\n");
            return drv_usage();
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
        drv_usage();
        return 0;
    }

    if (argc < 2) return drv_usage();
    const char *cmd = argv[1];

    {
        const char *resolved = resolve_command(cmd);
        if (resolved == COMMAND_AMBIGUOUS) return drv_usage();
        if (resolved != NULL) cmd = resolved;
    }

    if (strcmp(cmd, "emit-c") == 0)
        return cmd_emit_c_main(argc, argv);
    if (strcmp(cmd, "emit-h") == 0)
        return cmd_emit_h_main(argc, argv);
    if (strcmp(cmd, "check") == 0)
        return cmd_check_main(argc, argv);
    if (strcmp(cmd, "expand") == 0)
        return cmd_expand_main(argc, argv);
    if (strcmp(cmd, "audit-spans") == 0)
        return cmd_audit_spans_main(argc, argv);
    if (strcmp(cmd, "lsp") == 0)
        return cmd_lsp_main(argc, argv);
    if (strcmp(cmd, "mcp") == 0)
        return cmd_mcp_main(argc, argv);
    /* Debugger Phase 3: DAP server over the interpreter (JSON-RPC / stdio). */
    if (strcmp(cmd, "dap") == 0) {
        return cmd_dap();
    }
    if (strcmp(cmd, "trace") == 0)
        return cmd_trace_main(argc, argv);
    if (strcmp(cmd, "lsp-lite") == 0)
        return cmd_lsp_lite_main(argc, argv);
    if (strcmp(cmd, "build") == 0)
        return cmd_build_main(argc, argv);
    if (strcmp(cmd, "compile") == 0)
        return cmd_compile_main(argc, argv);
    if (strcmp(cmd, "link") == 0)
        return cmd_link_main(argc, argv);
    if (strcmp(cmd, "jit") == 0) {
        return cmd_jit(argc, argv);
    }
    if (strcmp(cmd, "emit-rt-split") == 0) {
        return cmd_emit_rt_split(argc, argv);
    }
    if (strcmp(cmd, "run") == 0)
        return cmd_run_main(argc, argv);
    if (strcmp(cmd, "repl") == 0)
        return cmd_repl_main(argc, argv);
    /* Tier 3: persistent fixture worker for the test suite. */
    if (strcmp(cmd, "worker") == 0) {
        return cmd_worker();
    }
    if (strcmp(cmd, "interpret") == 0 || strcmp(cmd, "--interpret") == 0)
        return cmd_interpret_main(argc, argv);
    if (strcmp(cmd, "debug") == 0)
        return cmd_debug_main(argc, argv);
    if (strcmp(cmd, "eval") == 0)
        return cmd_eval_main(argc, argv);
    if (strcmp(cmd, "doc") == 0)
        return cmd_doc_main(argc, argv);
    /* OD4: tur docs -- locate/open/serve the rendered guides and API pages. */
    if (strcmp(cmd, "docs") == 0)
        return cmd_docs(argc, argv);
    if (strcmp(cmd, "image-info") == 0)
        return cmd_image_info_main(argc, argv);
    if (strcmp(cmd, "image-verify") == 0)
        return cmd_image_verify_main(argc, argv);
    if (strcmp(cmd, "explain") == 0)
        return cmd_explain_main(argc, argv);
    if (strcmp(cmd, "format") == 0)
        return cmd_format_main(argc, argv);
    if (strcmp(cmd, "fmt") == 0)
        return cmd_fmt(argc, argv);
    if (strcmp(cmd, "parse-check") == 0)
        return cmd_parse_check_main(argc, argv);
    if (strcmp(cmd, "demangle") == 0)
        return cmd_demangle(argc, argv);
    if (strcmp(cmd, "test") == 0)
        return cmd_test_main(argc, argv);
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
