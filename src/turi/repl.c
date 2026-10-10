/* Phase S1: Interactive REPL for `tur repl`.
 *
 * Features:
 *  - libedit/readline line editing + persistent history (~/.tur_history)
 *  - Multi-line continuation prompt (`..`) while parentheses are unbalanced
 *  - Meta-commands: :help  :quit/:q  :type <expr>  :doc <sym>  :reload <file>  :run <file>
 *  - Colour diagnostics when stderr is a terminal (reuses src/diag.c)
 *  - Pretty-printer for TuriValue (extends turi_value_repr)
 */

/* Platform macros before any system headers */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif
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

#include "repl.h"
#include "eval.h"
#include "preload.h"       /* shared stdlib preload (macros + typed collections) */
#include "interpreter_natives.h"  /* wk_register_* interpreter native overrides */
#include "collections_native.h"   /* turi_register_collection_natives re-assert */
#include "spice_loader.h"  /* RP3: auto-discover + load the enclosing spice */
#include "elab_internal.h" /* :expand -- MacroDef registry + elab_expand_macro */
#include "ffi_thunk.h"     /* RP4: install per-export TuriNativeFn bindings */
#include "platform_fs.h"   /* setenv/unsetenv on Windows */
#include "lang_dialects.h" /* lang_traits: a dialect's prelude and name */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>   /* stat() -- logical-cwd identity check */
#include <unistd.h>

/* libedit readline-compatible API (ships with macOS and most Linux distros).
 * Gracefully degrade to fgets when not available at compile time. */
#ifdef TURI_HAVE_EDITLINE
#  include <editline/readline.h>
#endif

#ifdef TURI_HAVE_EDITLINE
/* E11: tab-completion — current REPL environment for the completion generator.
 * Updated whenever the env is recreated (e.g. after :reset). */
static TuriEnv *g_completion_env = NULL;
#endif

/* Compiler internals (CMake adds src/ to the include path) */
#include "arena.h"
#include "buf.h"
#include "diag.h"
#include "elab.h"
#include "expr.h"
#include "forms.h"
#include "reader.h"
#include "symbols.h"
#include "types.h"
/* Tutorial system */
#include "tutorial.h"

static char g_last_diag_code[16] = "";  /* "" = no recent code */

const char *turi_repl_get_last_diag_code(void) {
    return g_last_diag_code;
}

void turi_repl_set_last_diag_code(const char *code) {
    if (code) {
        strncpy(g_last_diag_code, code, sizeof(g_last_diag_code) - 1);
        g_last_diag_code[sizeof(g_last_diag_code) - 1] = '\0';
    } else {
        g_last_diag_code[0] = '\0';
    }
}

static void repl_diag_sink(struct TuriEnv *env, int level, const char *code,
                           const char *file, uint32_t line,
                           uint32_t col_start, uint32_t col_end,
                           const char *message, void *ud) {
    (void)env; (void)ud;

    /* Capture the first TUR-E#### or TUR-W#### code */
    if (g_last_diag_code[0] == '\0' && code && (strncmp(code, "TUR-E", 5) == 0 || strncmp(code, "TUR-W", 5) == 0)) {
        strncpy(g_last_diag_code, code, sizeof(g_last_diag_code) - 1);
        g_last_diag_code[sizeof(g_last_diag_code) - 1] = '\0';
    }

    /* Format and print the diagnostic to stderr since setting a sink suppresses standard rendering. */
    const char *lvl_str = "note";
    const char *color_code = "";
    bool use_color = isatty(STDERR_FILENO);
    if (level == 0)      { lvl_str = "error";   if (use_color) color_code = "\033[31m"; }
    else if (level == 1) { lvl_str = "warning"; if (use_color) color_code = "\033[33m"; }
    else if (level == 2) { lvl_str = "note";    if (use_color) color_code = "\033[36m"; }
    else if (level == 3) { lvl_str = "help";    if (use_color) color_code = "\033[32m"; }

    const char *reset_code = use_color ? "\033[0m" : "";

    if (file && file[0] != '\0') {
        if (code && code[0] != '\0') {
            fprintf(stderr, "%s%s:%u:%u: %s [%s]%s: %s\n",
                    color_code, file, line, col_start, lvl_str, code, reset_code, message);
        } else {
            fprintf(stderr, "%s%s:%u:%u: %s%s: %s\n",
                    color_code, file, line, col_start, lvl_str, reset_code, message);
        }

        /* Render source snippet if we can find the SourceFile */
        const SourceFile *sf = NULL;
        for (uint16_t id = 0; id < 64; id++) {
            const SourceFile *f = diag_source_file(id);
            if (!f) break;
            if (strcmp(f->path, file) == 0) {
                sf = f;
                break;
            }
        }
        if (sf) {
            Span span;
            span.file_id = sf->file_id;
            span.line = line;
            span.col_start = col_start;
            span.col_end = col_end;
            
            /* Helper to calculate byte offsets from line/col */
            uint32_t offset = 0;
            uint32_t curr_line = 1;
            uint32_t curr_col = 1;
            span.off_start = 0;
            span.off_end = 0;
            while (offset < sf->len) {
                if (curr_line == line && curr_col == col_start) {
                    span.off_start = offset;
                }
                if (curr_line == line && curr_col == col_end) {
                    span.off_end = offset;
                }
                if (sf->src[offset] == '\n') {
                    curr_line++;
                    curr_col = 1;
                } else {
                    curr_col++;
                }
                offset++;
            }
            if (span.off_end == 0) span.off_end = offset;

            diag_render_snippet(sf, span, NULL);
        }
    } else {
        if (code && code[0] != '\0') {
            fprintf(stderr, "%s%s [%s]%s: %s\n",
                    color_code, lvl_str, code, reset_code, message);
        } else {
            fprintf(stderr, "%s%s%s: %s\n",
                    color_code, lvl_str, reset_code, message);
        }
    }
}

/* -------------------------------------------------------------------------
 * TR2.4: per-env configuration for a freshly created REPL environment.
 *
 * The REPL is the canonical long-lived env: one TuriEnv serves the whole
 * session and turi_env_free is only called on :reset / :run. Scratch promotion
 * (turi-value-pool-scratch-promotion-plan) bounds the value pool for exactly
 * that shape -- at each top-level boundary it promotes escaping values into the
 * permanent pool and rewinds the scratch region, so per-turn transients do not
 * accumulate for the life of the session. It stays OFF by default for embedders
 * (the create/eval/free pattern needs no promotion), but the REPL always wants
 * it, and with TR2.1-TR2.3 having bounded the elaboration and source terms the
 * value pool is now the remaining unbounded one.
 *
 * The promotion walk is conservative: when a turn leaves live state it cannot
 * prove safe to relocate (a suspended generator, a captured continuation), it
 * simply declines to rewind that cycle rather than corrupting it. Measured at
 * ~100% rewind on ordinary REPL input (TR0).
 *
 * TUR_NO_SCRATCH_PROMOTION=1 opts out, for bisecting a suspected promotion bug.
 * ---------------------------------------------------------------------- */
static void repl_configure_env(TuriEnv *env) {
    if (!env) return;
    turi_env_set_diag_sink(env, repl_diag_sink, env);
    /* `(import m :refer [f])` at the prompt, then `(f ...)` on the next turn. */
    turi_env_set_toplevel_imports(env, true);
    const char *off = getenv("TUR_NO_SCRATCH_PROMOTION");
    if (!(off && *off && strcmp(off, "0") != 0))
        turi_env_set_scratch_promotion(env, true);
}

/* -------------------------------------------------------------------------
 * E11: Tab-completion generator (editline only)
 * ---------------------------------------------------------------------- */

#ifdef TURI_HAVE_EDITLINE
/* Known REPL meta-commands for colon-prefix completion. */
static const char *const k_meta_cmds[] = {
    ":help", ":quit", ":q",
    ":type", ":doc", ":expand", ":reload", ":load-string", ":run", ":reset", ":explain",
    ":cd", ":pwd",
    ":tutorial", ":next", ":prev", ":hint", ":skip",
    ":quit-tutorial", ":tutorial-progress",
    NULL
};

/* readline/editline generator: called with state=0 on first call for a
 * given prefix, then state>0 for subsequent calls.  Returns a malloc'd
 * match string or NULL when the list is exhausted. */
static char *tur_completion_generator(const char *text, int state) {
    static int meta_idx;
    static EnvBinding *cur_binding;
    size_t tlen = strlen(text);

    if (state == 0) {
        meta_idx   = 0;
        cur_binding = g_completion_env ? g_completion_env->globals : NULL;
    }

    if (text[0] == ':') {
        /* Complete meta-commands */
        while (k_meta_cmds[meta_idx]) {
            const char *m = k_meta_cmds[meta_idx++];
            if (strncmp(m, text, tlen) == 0) return strdup(m);
        }
        return NULL;
    }

    /* Complete environment bindings */
    while (cur_binding) {
        const char *name = cur_binding->name;
        cur_binding = cur_binding->next;
        if (strncmp(name, text, tlen) == 0) return strdup(name);
    }
    return NULL;
}
#endif /* TURI_HAVE_EDITLINE */

/* -------------------------------------------------------------------------
 * Shell-integration markers (OSC 133 semantic prompts, OSC 7 cwd reports).
 *
 * A host terminal (e.g. Trowel, iTerm2, WezTerm) uses these to track idle
 * vs. busy state without pattern-matching the prompt string.
 *
 * Enabled automatically when stdout is a TTY. Two env vars override that:
 *
 *   TUR_NO_SHELL_INTEGRATION=1  force off (a TTY that garbles the escapes)
 *   TUR_SHELL_INTEGRATION=1     force on  (stdout is a pipe, not a TTY)
 *
 * The force-on switch is the one that matters. A GUI host that drives the
 * REPL over pipes rather than a pty is exactly the consumer these markers
 * exist for, and it was the one consumer that could not get them: with no
 * marker ever arriving, a host has nothing to distinguish "evaluating" from
 * "waiting at the prompt" and has to latch busy forever as the safe default.
 * TUR_NO_SHELL_INTEGRATION still wins if both are set -- an explicit "off"
 * should never be overridden by an explicit "on".
 * ---------------------------------------------------------------------- */
static bool g_shell_integration = false;

/* A: prompt is about to be written -- the REPL is idle, awaiting input. */
static void repl_emit_prompt_marker(void) {
    if (!g_shell_integration) return;
    fputs("\x1b]133;A\x07", stdout);
    fflush(stdout);
}

/* C: input accepted, evaluation starting -- the REPL is busy.
 * D;<status>: evaluation finished, with 0 = ok and 1 = the form errored.
 *
 * A alone marks the idle edge but nothing marks the busy one, so a host
 * could see that a prompt had been written but not that work had started or
 * when it ended. C/D close that loop: A -> idle, C -> busy, D -> done.
 *
 * B (end of prompt / start of the typed command) is deliberately not
 * emitted. Placing it correctly means writing it between the prompt string
 * and the user's keystrokes, which is inside editline's own output -- it
 * would have to be embedded in the prompt behind \1..\2 non-printing guards
 * that libedit does not reliably honor, risking a visibly corrupted prompt
 * in a real terminal. B only serves command extraction, which no host here
 * needs (they send input programmatically); busy/idle needs A/C/D. */
static void repl_emit_exec_marker(void) {
    if (!g_shell_integration) return;
    fputs("\x1b]133;C\x07", stdout);
    fflush(stdout);
}

static void repl_emit_done_marker(int status) {
    if (!g_shell_integration) return;
    printf("\x1b]133;D;%d\x07", status);
    fflush(stdout);
}

/* -------------------------------------------------------------------------
 * Logical working directory (`pwd -L` semantics)
 *
 * getcwd(3) always answers with symlinks resolved. A host that launched us
 * in /tmp/project (itself a symlink, as /tmp is on macOS) therefore gets
 * told /private/tmp/project, which never string-matches the path it holds --
 * so a host comparing the two naively concludes the REPL is somewhere else
 * entirely and has to canonicalize both sides to recover.
 *
 * Shells solved this long ago: `pwd -L` reports $PWD when $PWD still names
 * the current directory, and only falls back to the resolved answer when it
 * does not. Same rule here, with the same safety check -- the candidate is
 * accepted only if it stats to the same (device, inode) as `.`, so we can
 * never report a path that is not this directory.
 * ---------------------------------------------------------------------- */

/* True if `path` names the directory we are actually in. */
static bool path_is_cwd(const char *path) {
    struct stat a, b;
    if (!path || path[0] != '/') return false;
    if (stat(path, &a) != 0 || stat(".", &b) != 0) return false;
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

/* Collapse "." and ".." components textually, the way a shell resolves them
 * against the logical path. Writes into `out` (which may not alias `in`).
 * Purely lexical -- popping ".." is wrong when the popped component was a
 * symlink, which is exactly what the path_is_cwd() check downstream catches. */
static void path_normalize(const char *in, char *out, size_t cap) {
    /* Component start offsets into `out`, for popping on "..". */
    size_t starts[PATH_MAX / 2];
    size_t depth = 0, o = 0;

    if (cap == 0) return;
    out[o++] = '/';

    for (const char *p = in; *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t seglen = (size_t)(p - seg);

        if (seglen == 1 && seg[0] == '.') continue;
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth > 0) o = starts[--depth];   /* pop */
            if (o == 0) o = 1;                    /* never above root */
            continue;
        }
        if (depth >= sizeof(starts) / sizeof(starts[0])) return; /* absurd */
        if (o + seglen + 1 >= cap) return;                       /* too long */
        starts[depth++] = o;
        if (o > 1) out[o++] = '/';
        memcpy(out + o, seg, seglen);
        o += seglen;
    }
    out[o ? o : 1] = '\0';
    if (o == 0) { out[0] = '/'; out[1] = '\0'; }
}

/* The working directory as the host would spell it: $PWD when it still names
 * this directory, otherwise getcwd(). Returns `out`, or NULL on failure. */
static char *repl_logical_cwd(char *out, size_t cap) {
    const char *pwd = getenv("PWD");
    if (pwd && pwd[0] == '/' && strlen(pwd) < cap) {
        char norm[PATH_MAX];
        path_normalize(pwd, norm, sizeof norm);
        if (path_is_cwd(norm) && strlen(norm) < cap) {
            memcpy(out, norm, strlen(norm) + 1);
            return out;
        }
    }
    return getcwd(out, cap);
}

/* Keep $PWD logical across `:cd` so the next report does not silently fall
 * back to the resolved path. Called after a successful chdir(); `target` is
 * the argument as the user wrote it. */
static void repl_update_logical_pwd(const char *target) {
    char cand[PATH_MAX];
    char norm[PATH_MAX];

    if (target[0] == '/') {
        if (strlen(target) >= sizeof cand) return;
        memcpy(cand, target, strlen(target) + 1);
    } else {
        const char *pwd = getenv("PWD");
        if (!pwd || pwd[0] != '/') return;
        if (snprintf(cand, sizeof cand, "%s/%s", pwd, target) >= (int)sizeof cand)
            return;
    }
    path_normalize(cand, norm, sizeof norm);
    /* Only adopt it if it really is where we landed. A ".." that crossed a
     * symlink normalizes to the wrong place; leaving $PWD alone makes
     * repl_logical_cwd fall back to getcwd(), which is always correct. */
    if (path_is_cwd(norm)) setenv("PWD", norm, 1);
    else                   unsetenv("PWD");
}

/* Report the current working directory as OSC 7, the de-facto standard
 * `file://<host>/<path>` notification.  A host that tracks it can keep its
 * own "REPL is rooted here" display honest across `:cd` without having to
 * restart the process or scrape output.
 *
 * Path characters outside the unreserved set are percent-encoded, so a
 * directory containing spaces or `#` survives the round trip. */
static void repl_emit_cwd_marker(void) {
    char cwd[PATH_MAX];

    if (!g_shell_integration) return;
    if (!repl_logical_cwd(cwd, sizeof cwd)) return;

    fputs("\x1b]7;file://", stdout);
    for (const unsigned char *p = (const unsigned char *)cwd; *p; p++) {
        if (isalnum(*p) || *p == '/' || *p == '-' || *p == '.' ||
            *p == '_' || *p == '~') {
            fputc((int)*p, stdout);
        } else {
            printf("%%%02X", *p);
        }
    }
    fputs("\x07", stdout);
    fflush(stdout);
}

/* -------------------------------------------------------------------------
 * Line input abstraction (editline when available, fgets fallback)
 * ---------------------------------------------------------------------- */

/* Read one line with the given prompt.  Returns a malloc'd string (caller
 * must free) or NULL on EOF/error.  Uses add_history when editline is
 * available and the line is non-empty. */
static char *repl_readline(const char *prompt) {
    repl_emit_prompt_marker();
#ifdef TURI_HAVE_EDITLINE
    char *line = readline(prompt);
    if (line && line[0] != '\0') add_history(line);
    return line; /* readline returns malloc'd memory */
#else
    if (isatty(STDIN_FILENO)) { printf("%s", prompt); fflush(stdout); }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
    return strdup(buf);
#endif
}

/* -------------------------------------------------------------------------
 * History file persistence
 * ---------------------------------------------------------------------- */

static char *history_path(void) {
#ifdef TURI_HAVE_EDITLINE
    const char *home = getenv("HOME");
    if (!home) return NULL;
    size_t len = strlen(home) + sizeof("/.tur_history");
    char *p = (char *)malloc(len);
    if (!p) return NULL;
    snprintf(p, len, "%s/.tur_history", home);
    return p;
#else
    return NULL;
#endif
}

/* -------------------------------------------------------------------------
 * Pretty-printer
 * Extends turi_value_repr with coloured output to a FILE* when appropriate.
 * ---------------------------------------------------------------------- */

/* ANSI colours — used only when output is a terminal. */
#define COL_RESET  "\033[0m"
#define COL_INT    "\033[36m"   /* cyan    */
#define COL_FLOAT  "\033[36m"
#define COL_BOOL   "\033[33m"   /* yellow  */
#define COL_CSTR   "\033[32m"   /* green   */
#define COL_FN     "\033[35m"   /* magenta */
#define COL_NIL    "\033[90m"   /* grey    */
#define COL_ERR    "\033[31m"   /* red     */

static void repl_print_value(TuriValue v, bool use_color) {
    char repr[512];
    turi_value_repr(repr, sizeof(repr), v);

    if (!use_color) {
        printf("=> %s\n", repr);
        return;
    }

    const char *col = COL_RESET;
    switch (v.tag) {
        case TURI_NIL:     col = COL_NIL;   break;
        case TURI_BOOL:    col = COL_BOOL;  break;
        case TURI_INT:     col = COL_INT;   break;
        case TURI_FLOAT:   col = COL_FLOAT; break;
        case TURI_CSTR:    col = COL_CSTR;  break;
        case TURI_CLOSURE:     col = COL_FN;    break;
        case TURI_ERROR:       col = COL_ERR;   break;
        case TURI_EFFECT_CONT: col = COL_FN;    break;
        case TURI_STRUCT:      col = COL_RESET; break;
        case TURI_THROW:       col = COL_ERR;   break;
        case TURI_FUTURE:      col = COL_RESET; break;
        case TURI_REF:         col = COL_RESET; break;
        case TURI_STRUCT_TYPE: col = COL_NIL;   break;
        case TURI_GEN:         col = COL_RESET; break;
        case TURI_HANDLER:     col = COL_RESET; break;
        case TURI_REJECTION:   col = COL_ERR;   break;
        case TURI_SYNTAX:      col = COL_RESET; break;
    }
    printf("=> %s%s%s\n", col, repr, COL_RESET);
}

/* -------------------------------------------------------------------------
 * :type <expr>  — elaborate and print the inferred type without evaluating
 * ---------------------------------------------------------------------- */

static void cmd_type(TuriEnv *env, const char *expr_src) {
    /* Build combined source so previous definitions are visible */
    Buf combined;
    buf_init(&combined);
    if (env->src_acc.len > 0) {
        buf_write(&combined, env->src_acc.data, env->src_acc.len);
    }
    buf_puts(&combined, expr_src);
    buf_putc(&combined, '\0');

    Arena    arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    diag_reset();
    SourceFile sfile = {0};  /* clear xform_map/orig_src so diag rendering is safe */
    sfile.path        = "<type>";
    sfile.src         = combined.data;
    sfile.len         = combined.len - 1;
    sfile.file_id     = 0;
    sfile.reader_type = env->reader_type;
    sfile.lang        = env->lang;   /* saffron-lang-plan S1 */
    diag_register_file(&sfile);

    uint32_t nforms = 0;
    /* RM Q#5: share the session registry with `:type` so it sees macros
     * defined earlier in the REPL. */
    Form **forms = read_all_with_registry(&arena, &st, &sfile,
                                          env->reader_macros, &nforms);
    if (!forms || diag_had_error()) goto cleanup;

    {
        Expr *prog = elaborate_program(&arena, &st, forms, nforms,
                                       /*stdlib_prefix=*/0, ".",
                                       /*separate_compilation=*/false,
                                       /*sandboxed=*/false,
                                       /*tc_env=*/NULL,
                                       /*include_dirs=*/NULL,
                                       /*n_include_dirs=*/0,
                                       /*out_n_fsd=*/NULL,
                                       /* RM transitive: `:type` shares
                                        * the session registry. */
                                       env->reader_macros);
        if (!prog || diag_had_error()) goto cleanup;

        /* The last top-level item is the expression we care about */
        uint32_t n = prog->as.program.n;
        if (n == 0) { printf(":type — empty expression\n"); goto cleanup; }

        Expr *last = prog->as.program.items[n - 1];
        printf(": %s\n", type_name(last->type));
    }

cleanup:
    /* symtab_init callocs the bucket array; it was never freed here, and
     * LeakSanitizer reported it whenever no stale stack word happened to
     * still point at it (tur_repl_smoke's `:type` checks, Debug build). */
    symtab_free(&st);
    arena_free(&arena);
    buf_free(&combined);
}

/* -------------------------------------------------------------------------
 * :expand <form> -- expand-1: expand the form's head macro ONCE and print
 * the result (macro-system-direction-plan, the deferred REPL expand-1).
 *
 * Same setup as cmd_type -- re-read the accumulated session source so
 * macros defined at earlier prompts are registered -- but the elaboration
 * runs through a throwaway ElabSession so the macro REGISTRY survives the
 * call and the given form can be expanded by hand.  defmacro* macros work
 * too: their closures live in the throwaway session's own macro env,
 * recreated (with the stdlib preload) for this command and torn down with
 * the session.
 * ------------------------------------------------------------------------- */
static void cmd_expand(TuriEnv *env, const char *expr_src) {
    Buf combined;
    buf_init(&combined);
    if (env->src_acc.len > 0)
        buf_write(&combined, env->src_acc.data, env->src_acc.len);
    size_t prior_len = combined.len;
    buf_puts(&combined, expr_src);
    buf_putc(&combined, '\0');

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    diag_reset();
    SourceFile sfile = {0};
    sfile.path        = "<expand>";
    sfile.src         = combined.data;
    sfile.len         = combined.len - 1;
    sfile.file_id     = 0;
    sfile.reader_type = env->reader_type;
    sfile.lang        = env->lang;   /* saffron-lang-plan S1 */
    diag_register_file(&sfile);

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &sfile,
                                          env->reader_macros, &nforms);
    if (!forms || nforms == 0 || diag_had_error()) {
        if (!diag_had_error()) printf(":expand -- empty expression\n");
        goto cleanup_noses;
    }

    {
        /* The LAST top-level form is the one to expand; everything before
         * it (the accumulated session source) elaborates first so its
         * macros register into the session. */
        Form *target = forms[nforms - 1];
        if (target->span.off_start < prior_len) {
            printf(":expand -- give one form to expand\n");
            goto cleanup_noses;
        }
        ElabSession *sess = elab_session_new();
        if (nforms > 1) {
            (void)elaborate_program_session(&arena, &st, forms, nforms - 1,
                                            /*stdlib_prefix=*/0, ".",
                                            /*separate_compilation=*/false,
                                            /*sandboxed=*/false,
                                            /*tc_env=*/NULL, NULL, 0, NULL,
                                            env->reader_macros, sess);
            /* Prior-source errors are the prompt's business, not this
             * command's; the macro registry is populated regardless of a
             * failed later form.  (A failed session must be discarded for
             * further ELABORATION -- expansion only reads the registry.) */
            diag_reset();
            diag_register_file(&sfile);
        }
        Elab *e = (Elab *)sess;
        if (target->tag != F_LIST || target->as.list.len == 0 ||
            target->as.list.items[0]->tag != F_SYM) {
            printf(":expand -- not a macro call\n");
            elab_session_free(sess);
            goto cleanup_noses;
        }
        MacroDef *macro = elab_lookup_macro(e, target->as.list.items[0]->as.sym);
        if (!macro) {
            printf(":expand -- '%s' is not a macro here\n",
                   target->as.list.items[0]->as.sym->name);
            elab_session_free(sess);
            goto cleanup_noses;
        }
        uint32_t n_args = target->as.list.len - 1;
        Form **args = (n_args == 0) ? NULL
            : (Form **)arena_alloc(&arena, n_args * sizeof(Form *));
        for (uint32_t i = 0; i < n_args; i++)
            args[i] = target->as.list.items[1 + i];
        Form *expanded = elab_expand_macro(e, macro, args, n_args);
        if (expanded) {
            Buf out;
            buf_init(&out);
            form_print(&out, expanded);
            printf("%.*s\n", (int)out.len, out.data);
            buf_free(&out);
        }
        elab_session_free(sess);
    }

cleanup_noses:
    symtab_free(&st);   /* same missing free as cmd_type's */
    arena_free(&arena);
    buf_free(&combined);
}

/* -------------------------------------------------------------------------
 * :doc <sym>  — print a brief description for known builtins / env bindings
 * ---------------------------------------------------------------------- */

/* turi_doc_lookup_builtin lives in turi/docstrings.c. */


static void cmd_doc(TuriEnv *env, const char *sym) {
    /* Check if the symbol is bound in the env */
    TuriValue v = turi_env_get(env, sym);
    if (!turi_is_error(v)) {
        char repr[256];
        turi_value_repr(repr, sizeof(repr), v);
        printf("%s = %s\n", sym, repr);
        return;
    }

    const char *d = turi_doc_lookup_builtin(sym);
    if (d) {
        printf("%s\n", d);
        return;
    }

    printf("no documentation for '%s'\n", sym);
}

static void cmd_explain(TuriEnv *env, const char *arg) {
    (void)env;
    if (arg && arg[0] != '\0') {
        /* Normalise ARG to upper case */
        char code[16];
        size_t len = strlen(arg);
        if (len >= sizeof(code)) {
            printf("unknown diagnostic code '%s'\n", arg);
            return;
        }
        for (size_t i = 0; i < len; i++) {
            char c = arg[i];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            code[i] = c;
        }
        code[len] = '\0';

        if (diag_looks_like_code(code)) {
            DiagCode dc = diag_code_from_string(code);
            if (dc != DIAG_CODE_NONE) {
                if (!diag_explain(dc, stdout)) {
                    printf("unknown diagnostic code '%s'\n", code);
                }
            } else {
                printf("unknown diagnostic code '%s'\n", code);
            }
        } else {
            printf("unknown diagnostic code '%s'\n", arg);
        }
    } else {
        /* Bare :explain */
        if (g_last_diag_code[0] != '\0') {
            DiagCode dc = diag_code_from_string(g_last_diag_code);
            if (dc != DIAG_CODE_NONE) {
                diag_explain(dc, stdout);
            } else {
                printf(":explain -- no recent diagnostic to explain. Try :explain TUR-E#### for a specific code.\n");
            }
        } else {
            printf(":explain -- no recent diagnostic to explain. Try :explain TUR-E#### for a specific code.\n");
        }
    }
}

/* -------------------------------------------------------------------------
 * Evaluating a file into the session without changing the session's reader
 * ---------------------------------------------------------------------- */

/* The (reader, language) a file reads under on its own: its extension, with
 * a `#lang` line taking over when the extension says plain Turmeric -- the
 * precedence every CLI entry point uses (detect_and_adjust_lang).  False when
 * the file cannot be read; the caller then lets turi_eval_file report it. */
static bool repl_file_dialect(const char *path, ReaderType *rt_out,
                              LangDialect *dl_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char head[4096];
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    const char *src = head;
    /* A shebang line comes before the directive (turi_eval strips it too). */
    if (n >= 2 && head[0] == '#' && head[1] == '!') {
        const char *nl = memchr(head, '\n', n);
        if (!nl) n = 0, src = head;
        else { n -= (size_t)(nl + 1 - head); src = nl + 1; }
    }
    const char *rest = src;
    size_t rest_len = n;
    LangDialect dl = LANG_TURMERIC;
    ReaderType lang_rt = detect_lang_dialect(src, n, &rest, &rest_len,
                                             NULL, NULL, &dl);
    ReaderType ext_rt = reader_type_from_extension(path);
    ReaderType rt = READER_TURMERIC;
    if (ext_rt != READER_TURMERIC) rt = ext_rt;
    else if (rest != src) rt = lang_rt;
    LangDialect ext_dl = lang_dialect_from_extension(path);
    if (ext_dl != LANG_TURMERIC) dl = ext_dl;
    *rt_out = rt;
    *dl_out = dl;
    return true;
}

/* Evaluate `path` into `env` for :run / :reload.
 *
 * run-on-sweet-file-switches-the-session-reader: turi_eval_file applies the
 * file's reader to the SESSION -- it has to, because the session re-reads
 * its accumulated source under env->reader_type -- so `:run foo.tur.sweet`
 * left a `turmeric` session reading every later line as sweet-exp, and
 * `:reload foo.tur.sweet` reset the session to its prelude first, discarding
 * everything typed before it.  A file whose reader differs from the
 * session's is evaluated as a `(load "...")` form instead: `load` reads the
 * file under its own reader, and what the session accumulates is the form,
 * which reads the same under any of them.
 *
 * A file in another LANGUAGE is a different matter: a Scheme file needs the
 * Scheme prelude and lowering, which a Turmeric session does not have, so it
 * is declined by name rather than half-run.  Returns an error value then. */
static TuriValue repl_eval_file_keeping_reader(TuriEnv *env, const char *path,
                                               const char *verb) {
    ReaderType  file_rt = READER_TURMERIC;
    LangDialect file_dl = LANG_TURMERIC;
    if (!repl_file_dialect(path, &file_rt, &file_dl) ||
        file_rt == env->reader_type)
        return turi_eval_file(env, path);

    if (file_dl != env->lang && file_dl != LANG_TURMERIC) {
        char base[64];
        lang_base_spelling_of(file_dl, file_rt, base, sizeof(base));
        fprintf(stderr,
                "%s: %s is %s, and this session is not -- start one with "
                "`tur repl --lang %s` to %s it\n",
                verb, path, base, base, verb);
        return turi_error("dialect mismatch");
    }

    Buf form;
    buf_init(&form);
    buf_puts(&form, "(load \"");
    for (const char *p = path; *p; p++) {
        if (*p == '"' || *p == '\\') buf_putc(&form, '\\');
        buf_putc(&form, *p);
    }
    buf_puts(&form, "\")");
    buf_putc(&form, '\0');
    TuriValue v = turi_eval(env, form.data);
    buf_free(&form);
    return v;
}

/* -------------------------------------------------------------------------
 * :reload <file>  — evaluate a source file into the current environment
 * ---------------------------------------------------------------------- */

static void cmd_reload(TuriEnv *env, const char *path) {
    TuriValue result = repl_eval_file_keeping_reader(env, path, ":reload");
    if (turi_is_error(result)) {
        const char *msg = turi_error_message(result);
        if (msg &&
            strcmp(msg, "parse error") != 0 &&
            strcmp(msg, "elaboration error") != 0 &&
            strcmp(msg, "dialect mismatch") != 0) {
            fprintf(stderr, "reload error: %s\n", msg);
        }
    } else {
        printf("reloaded %s\n", path);
    }
}

/* Reinstate the interactive prompt's stdlib surface on a freshly created env.
 * Startup does this inline (turi_env_new -> preload macros/collections/
 * typeclasses -> re-register the inline-C native overrides -> reload native);
 * :reset and :run (cmd_run) recreate the env and MUST run the same sequence or
 * the session loses everything the preload bound -- `#map{}`/`#set{}`, the
 * typeclass Show instances, and the carrier list helpers (list-head/list-tail),
 * which then warn TUR-W0040 and, for the non-native ones (hamt-of, ...), fail
 * at runtime. Keeping the three call sites in one helper stops them drifting.
 * NOTE: spice auto-discovery (RP3) is intentionally NOT re-run here -- it is
 * cwd-dependent and prints its own banner; only the always-on stdlib preload
 * belongs in the shared reinit. */
static void repl_preload_stdlib_and_natives(TuriEnv *env) {
    const char *stdlib_root = getenv("TUR_STDLIB_DIR");
    turi_env_preload_macros(env, stdlib_root);
    /* Typed native-function stubs, in the SAME slot the `--interpret` path uses
     * (after macros, before collections).  Without these, `cons`/`head`/`tail`
     * resolve to the elaborator builtin the tree-walker cannot execute, so
     * `(list-head (cons 65 (cons 66 0)))` returned nil at the prompt while the
     * compiled and `--interpret` paths gave 65.  See
     * docs/archive/repl-list-head-over-cons-returns-nil.md. */
    turi_env_preload_native_stubs(env);
    turi_env_preload_collections(env, stdlib_root);
    turi_env_preload_typeclasses(env, stdlib_root);
    /* Pin everything the preload just accumulated so a `#lang` switch at the
     * prompt truncates back to here instead of emptying src_acc and taking the
     * stdlib with it (web-repl-lang-switch-drops-stdlib). */
    turi_env_pin_prelude(env);
    turi_env_register_interpreter_natives(env);
    /* Re-assert collection natives (vec/set/map/hamt) after preload -- see
     * the matching call in main.c for why (turi_register_collection_natives
     * is otherwise only ever registered once, before any preload runs). */
    turi_register_collection_natives(env);
    tur_ffi_register_reload_native(env);
}

/* -------------------------------------------------------------------------
 * :load-string "<src>"  -- evaluate source handed over directly.
 *
 * The prompt is line-oriented, so a host wanting to run a multi-line
 * selection had no way to say so: it wrote the region to a scratch file and
 * sent `(load "...")`, paying a disk round-trip (and leaving temp files
 * behind) for something the evaluator can already do from memory --
 * turi_eval_file is itself just read-file plus turi_eval.
 *
 * The argument is one double-quoted literal with C-style escapes, so an
 * arbitrary region collapses onto the single line the prompt reads: newlines
 * travel as \n. That is trivial for a host to produce and unambiguous to
 * parse, which a bare unquoted tail would not be.
 * ---------------------------------------------------------------------- */

/* Decode the quoted literal starting at *pp (which must point at the opening
 * quote). Returns a malloc'd string and advances *pp past the closing quote,
 * or NULL if the literal is unterminated. */
static char *unquote_literal(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;

    size_t cap = strlen(p) + 1;
    char  *out = (char *)malloc(cap);
    size_t o = 0;
    if (!out) return NULL;

    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n':  out[o++] = '\n'; break;
                case 't':  out[o++] = '\t'; break;
                case 'r':  out[o++] = '\r'; break;
                case '0':  out[o++] = '\0'; break;
                case '"':  out[o++] = '"';  break;
                case '\\': out[o++] = '\\'; break;
                default:   out[o++] = *p;   break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    if (*p != '"') { free(out); return NULL; }  /* unterminated */
    out[o] = '\0';
    *pp = p + 1;
    return out;
}

/* r7rs-lang-plan R9: a fresh session in dialect `d` read by `r`.
 *
 * The language's prelude (LangTraits.prelude) is part of the stdlib preload,
 * and the preload is PINNED -- a `#lang` switch at the prompt rewinds to it.
 * So the dialect has to be seeded BEFORE the preload runs, and a switch to a
 * dialect whose prelude differs cannot be a rewind: it needs a new preload.
 * `#lang r7rs` at the prompt used to switch the reader and the language and
 * keep Turmeric's preload, so `(display ...)` met Turmeric's `display` method
 * rather than the R7RS prelude's.  Every route that builds a session --
 * startup, :reset, :run, and a prelude-changing `#lang` -- comes through here.
 * The fields are assigned directly rather than through
 * turi_env_apply_lang_dialect, which rewinds to a preload that does not exist
 * yet. */
static TuriEnv *repl_fresh_env(LangDialect d, ReaderType r) {
    TuriEnv *env = turi_env_new();
    if (!env) return NULL;
    repl_configure_env(env);   /* TR2.4 */
    env->lang        = d;
    env->reader_type = r;
    g_lang_prelude   = lang_traits(d)->prelude;
    repl_preload_stdlib_and_natives(env);
    return env;
}

/* -------------------------------------------------------------------------
 * :run <file>  -- DrRacket-style "press Run" semantic.
 *
 * Resets the env (mirrors :reset) and then loads the file, so re-pressing
 * Run on an edited file cleanly redefines bindings instead of tripping
 * the elaborator's duplicate-defn check against the source accumulator
 * (see docs/notes/tur-repl-reload-semantics.md). If the file defines
 * (main), invoke it automatically. Returns the (possibly new) env via
 * *env_io; callers must update their local and any completion handle.
 * ---------------------------------------------------------------------- */

static void cmd_run(TuriEnv **env_io, const char *path) {
    if (!env_io || !*env_io) return;

    TuriEnv *env = *env_io;
    /* r7rs-lang-plan R9: the fresh session keeps the session's dialect. */
    LangDialect keep_lang = env->lang;
    ReaderType  keep_rt   = env->reader_type;
    turi_env_free(env);
    env = repl_fresh_env(keep_lang, keep_rt);
    if (!env) {
        fprintf(stderr, "tur repl: failed to allocate fresh environment\n");
        *env_io = NULL;
        return;
    }
    *env_io = env;

    printf(";; run: %s\n", path);
    fflush(stdout);

    TuriValue result = repl_eval_file_keeping_reader(env, path, ":run");
    if (turi_is_error(result)) {
        const char *msg = turi_error_message(result);
        if (msg &&
            strcmp(msg, "parse error") != 0 &&
            strcmp(msg, "elaboration error") != 0 &&
            strcmp(msg, "dialect mismatch") != 0) {
            fprintf(stderr, "run error: %s\n", msg);
        }
        return;
    }

    /* Auto-invoke (main) if it resolves to a closure. Unbound or non-closure
     * is silent: not every script defines main, and the top-level forms
     * already ran above. */
    TuriValue mainv = turi_env_get(env, "main");
    if (mainv.tag == TURI_CLOSURE) {
        TuriValue r = turi_call(env, mainv, NULL, 0);
        turi_run_pending_defers(env);
        if (turi_is_error(r)) {
            const char *msg = turi_error_message(r);
            if (msg) fprintf(stderr, "main: %s\n", msg);
        } else if (r.tag != TURI_NIL) {
            char repr[1024];
            turi_value_repr(repr, sizeof(repr), r);
            printf("=> %s\n", repr);
        }
    }
    printf(";; ready\n");
    fflush(stdout);
}

/* -------------------------------------------------------------------------
 * Help text
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * 2h: detect type-level definition forms to show a sentinel instead of nil
 * ---------------------------------------------------------------------- */

/* Scan src for a type-level definition like (defstruct Foo ...).
 * Returns true and writes kind + name if found, false otherwise. */
static bool detect_type_form(const char *src,
                              char *kind, size_t ksz,
                              char *name, size_t nsz) {
    /* Skip leading whitespace and optional opening paren */
    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r') src++;
    if (*src == '(') src++;
    while (*src == ' ' || *src == '\t') src++;

    static const struct { const char *kw; const char *kind; } forms[] = {
        {"defstruct ",    "struct-type"},
        {"defdata ",      "adt-type"},
        {"defgadt ",      "adt-type"},
        {"deftype ",      "type-alias"},
        {"defmacro ",     "macro"},
        {"defeffect ",    "effect"},
        {"defclass ",     "typeclass"},
        {"definstance ",  "instance"},
        {NULL, NULL}
    };

    for (int i = 0; forms[i].kw; i++) {
        size_t klen = strlen(forms[i].kw);
        if (strncmp(src, forms[i].kw, klen) != 0) continue;

        snprintf(kind, ksz, "%s", forms[i].kind);
        const char *p = src + klen;
        while (*p == ' ' || *p == '\t') p++;

        if (strcmp(forms[i].kind, "instance") == 0) {
            /* (definstance Typeclass Type ...) — include both names */
            char tc[32] = {0}, ty[32] = {0};
            int ti = 0;
            while (*p && *p != ' ' && *p != ')' && ti < 31) tc[ti++] = *p++;
            while (*p == ' ') p++;
            int yi = 0;
            while (*p && *p != ' ' && *p != ')' && yi < 31) ty[yi++] = *p++;
            snprintf(name, nsz, "%s/%s", tc, ty);
        } else {
            size_t ni = 0;
            while (*p && *p != ' ' && *p != ')' && *p != ']' && *p != '[' && ni < nsz - 1)
                name[ni++] = *p++;
            name[ni] = '\0';
        }
        return name[0] != '\0';
    }
    return false;
}

static void print_help(void) {
    printf(
        "Meta-commands:\n"
        "  :help               show this help\n"
        "  :quit  :q           exit the REPL\n"
        "  :type <expr>        print inferred type without evaluating\n"
        "  :expand <form>      expand the form's head macro once and print it\n"
        "  :doc  <sym>         print documentation for a symbol or builtin\n"
        "  :reload <file>      evaluate a .tur file into the current session\n"
        "  :load-string \"<src>\"  evaluate source directly (\\n for newlines)\n"
        "  :run <file>         reset session, load file, auto-invoke (main)\n"
        "  :reset              clear session and start fresh\n"
        "  :pwd                print the working directory\n"
        "  :cd [dir]           change the working directory (bare :cd goes home)\n"
        "  :explain [code]     explain the most recent error, or a TUR-E#### code\n"
        "\n"
        "Tutorial commands:\n"
        "  :tutorial              list available tutorials\n"
        "  :tutorial <name>       start a tutorial\n"
        "  :tutorial <name> <n>   start a tutorial at step n\n"
        "  :next                  go to next step\n"
        "  :prev                  go to previous step\n"
        "  :hint                  show hint for current step\n"
        "  :skip                  skip current step\n"
        "  :quit-tutorial         exit tutorial mode\n"
        "  :tutorial-progress     show progress in current tutorial\n"
        "\n"
        "Expressions are evaluated and the result printed.\n"
        "Definitions (defn, def) persist across expressions.\n"
        "Multi-line input: keep typing when parentheses are open; use\n"
        "  an empty line to cancel an incomplete expression.\n"
    );
}

/* -------------------------------------------------------------------------
 * Main REPL entry point
 * ---------------------------------------------------------------------- */

/* Tutorial state for the REPL session */
static TutorialState *g_tutorial_state = NULL;

/* -------------------------------------------------------------------------
 * Tutorial meta-commands
 * ---------------------------------------------------------------------- */

static void print_tutorial_help(void) {
    printf(
        "Tutorial meta-commands:\n"
        "  :tutorial              list available tutorials\n"
        "  :tutorial <name>       start a tutorial\n"
        "  :tutorial <name> <n>   start a tutorial at step n\n"
        "  :next                  go to next step\n"
        "  :prev                  go to previous step\n"
        "  :hint                  show hint for current step\n"
        "  :skip                  skip current step\n"
        "  :quit-tutorial         exit tutorial mode\n"
        "  :tutorial-progress     show progress in current tutorial\n"
    );
}

static void cmd_tutorial_list(void) {
    int count = 0;
    Tutorial **tutorials = tutorial_load_all(&count);
    
    if (count == 0) {
        printf("No tutorials available.\n");
        return;
    }
    
    printf("Available tutorials:\n");
    for (int i = 0; i < count; i++) {
        Tutorial *t = tutorials[i];
        char completed_marker = tutorial_is_completed(g_tutorial_state, t->id) ? 'v' : ' ';
        printf("  %c %s - %s (%d steps, difficulty %d/5)\n",
               completed_marker, t->id, t->description, t->step_count, t->difficulty);
    }
}

static void cmd_tutorial_start(const char *name, int start_step) {
    if (!name || name[0] == '\0') {
        printf(":tutorial requires a tutorial name\n");
        return;
    }
    
    if (!tutorial_start(g_tutorial_state, name)) {
        printf("unknown tutorial '%s' — use :tutorial to list available tutorials\n", name);
        return;
    }
    
    if (start_step > 0) {
        tutorial_jump(g_tutorial_state, start_step - 1);
    }
    
    Tutorial *t = tutorial_get_current_tutorial(g_tutorial_state);
    TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
    
    printf("Starting tutorial: %s\n", t->title);
    printf("Step %d/%d: %s\n", tutorial_get_current_step_index(g_tutorial_state) + 1,
           tutorial_get_step_count(g_tutorial_state), step->title);
    printf("Instruction: %s\n", step->instruction);
}

static void cmd_tutorial_next(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode. Use :tutorial <name> to start a tutorial.\n");
        return;
    }
    
    if (tutorial_next(g_tutorial_state)) {
        TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
        printf("Step %d/%d: %s\n", tutorial_get_current_step_index(g_tutorial_state) + 1,
               tutorial_get_step_count(g_tutorial_state), step->title);
        printf("Instruction: %s\n", step->instruction);
    } else {
        printf("You have completed all steps in this tutorial!\n");
        printf("Use :tutorial-progress to see your progress.\n");
    }
}

static void cmd_tutorial_prev(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode. Use :tutorial <name> to start a tutorial.\n");
        return;
    }
    
    if (tutorial_prev(g_tutorial_state)) {
        TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
        printf("Step %d/%d: %s\n", tutorial_get_current_step_index(g_tutorial_state) + 1,
               tutorial_get_step_count(g_tutorial_state), step->title);
        printf("Instruction: %s\n", step->instruction);
    } else {
        printf("Already at the first step.\n");
    }
}

static void cmd_tutorial_hint(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode. Use :tutorial <name> to start a tutorial.\n");
        return;
    }
    
    TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
    int hint_count = tutorial_get_hint_count(step);
    
    if (hint_count == 0) {
        printf("No hints available for this step.\n");
        return;
    }
    
    /* Show all hints */
    printf("Hints:\n");
    for (int i = 0; i < hint_count; i++) {
        const char *hint = tutorial_get_hint(step, i);
        printf("  %d. %s\n", i + 1, hint);
    }
}

static void cmd_tutorial_skip(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode. Use :tutorial <name> to start a tutorial.\n");
        return;
    }
    
    TutorialStep *step = tutorial_get_current_step(g_tutorial_state);

    printf("Skipping step: %s\n", step->title);
    cmd_tutorial_next();
}

static void cmd_tutorial_quit(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode.\n");
        return;
    }
    
    Tutorial *t = tutorial_get_current_tutorial(g_tutorial_state);
    printf("Exiting tutorial: %s\n", t->title);
    g_tutorial_state->in_tutorial = false;
    g_tutorial_state->tutorial = NULL;
    g_tutorial_state->current_step = 0;
}

static void cmd_tutorial_progress(void) {
    if (!g_tutorial_state->in_tutorial) {
        printf("Not in tutorial mode.\n");
        return;
    }
    
    Tutorial *t = tutorial_get_current_tutorial(g_tutorial_state);
    int progress = tutorial_get_progress(g_tutorial_state);
    int current = tutorial_get_current_step_index(g_tutorial_state) + 1;
    int total = tutorial_get_step_count(g_tutorial_state);
    
    printf("Tutorial: %s\n", t->title);
    printf("Progress: %d%% (%d/%d steps)\n", progress, current, total);
    
    TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
    printf("Current step: %s\n", step->title);
    printf("Instruction: %s\n", step->instruction);
}

/* -------------------------------------------------------------------------
 * Tutorial integration with evaluation
 * ---------------------------------------------------------------------- */

/* Check if the input matches the current tutorial step */
static bool check_tutorial_step(TuriEnv *env, const char *input) {
    if (!g_tutorial_state->in_tutorial) return false;
    
    TutorialStep *step = tutorial_get_current_step(g_tutorial_state);
    
    if (tutorial_is_step_complete(g_tutorial_state, input)) {
        printf("✓ %s\n", step->success_message);
        
        /* If there's a verify expression, evaluate it */
        if (step->verify_expr) {
            TuriValue result = turi_eval(env, step->verify_expr);
            if (turi_is_error(result)) {
                printf("Verification failed: %s\n", turi_error_message(result));
            } else {
                char repr[256];
                turi_value_repr(repr, sizeof(repr), result);
                printf("Verification: %s\n", repr);
            }
        }
        
        /* Mark step as completed */
        g_tutorial_state->step_completed = true;
        
        /* Auto-advance to next step */
        if (tutorial_next(g_tutorial_state)) {
            TutorialStep *next_step = tutorial_get_current_step(g_tutorial_state);
            printf("\nStep %d/%d: %s\n", tutorial_get_current_step_index(g_tutorial_state) + 1,
                   tutorial_get_step_count(g_tutorial_state), next_step->title);
            printf("Instruction: %s\n", next_step->instruction);
        } else {
            /* Completed all steps */
            Tutorial *t = tutorial_get_current_tutorial(g_tutorial_state);
            printf("\n✓ Tutorial '%s' completed!\n", t->title);
            tutorial_mark_completed(g_tutorial_state);
            g_tutorial_state->in_tutorial = false;
            g_tutorial_state->tutorial = NULL;
        }
        
        return true;
    }
    
    return false;
}

int turi_repl_run(bool watch_mode) {
    bool use_color = isatty(STDOUT_FILENO) && isatty(STDERR_FILENO);
    const char *no_shell_integ = getenv("TUR_NO_SHELL_INTEGRATION");
    const char *yes_shell_integ = getenv("TUR_SHELL_INTEGRATION");
    bool force_off = no_shell_integ  && strcmp(no_shell_integ,  "1") == 0;
    bool force_on  = yes_shell_integ && strcmp(yes_shell_integ, "1") == 0;
    g_shell_integration = !force_off && (force_on || isatty(STDOUT_FILENO));

    /* Tell the host where we start, so it does not have to assume the cwd it
     * launched us with is still current. */
    repl_emit_cwd_marker();

    turi_init(use_color);
    
    /* Initialize tutorial system */
    tutorial_init();
    g_tutorial_state = tutorial_state_new();
    if (!g_tutorial_state) {
        fprintf(stderr, "tur repl: failed to initialize tutorial state\n");
    }

#ifndef TUR_VERSION
#define TUR_VERSION "unknown"
#endif
    if (use_color) {
        printf(COL_BOOL
               "\xe2\x96\x97            \xe2\x96\x98  \n"
               "\xe2\x96\x9c\xe2\x96\x98\xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x9b\xe2\x96\x9b\xe2\x96\x8c\xe2\x96\x88\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\n"
               "\xe2\x96\x90\xe2\x96\x96\xe2\x96\x99\xe2\x96\x8c\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\n"
               COL_RESET "\n");
    } else {
        printf("\xe2\x96\x97            \xe2\x96\x98  \n"
               "\xe2\x96\x9c\xe2\x96\x98\xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x9b\xe2\x96\x9b\xe2\x96\x8c\xe2\x96\x88\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\xe2\x96\x8c\xe2\x96\x9b\xe2\x96\x98\n"
               "\xe2\x96\x90\xe2\x96\x96\xe2\x96\x99\xe2\x96\x8c\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\xe2\x96\x8c \xe2\x96\x8c\xe2\x96\x99\xe2\x96\x96\n"
               "\n");
    }
    printf("Turmeric v" TUR_VERSION "  (type :help for help, :quit to exit)\n");
    fflush(stdout);

    /* saffron-lang-plan S8 / r7rs-lang-plan R9: `tur repl --lang <base>`
     * (main.c validated the token), seeded before the preload -- see
     * repl_fresh_env. */
    LangDialect start_lang = LANG_TURMERIC;
    ReaderType  start_rt   = READER_TURMERIC;
    if (g_repl_start_lang)
        (void)lang_base_lookup(g_repl_start_lang, strlen(g_repl_start_lang),
                               &start_lang, &start_rt);
    TuriEnv *env = repl_fresh_env(start_lang, start_rt);
    if (!env) {
        fprintf(stderr, "tur repl: failed to create eval environment\n");
        return 1;
    }

    /* The stdlib preload (macros, native stubs, collections, typeclasses,
     * natives, `(reload)`) ran inside repl_fresh_env above: see
     * repl_preload_stdlib_and_natives, and web-repl-missing-stdlib-preload
     * for why the prompt must match the `--interpret` path. */

    /* RP3: auto-discover an enclosing spice project and load its
     * shared library. Skipped silently when:
     *   - the user sets TUR_NO_AUTO_SPICE=1, or
     *   - no build.tur exists walking up from cwd.
     * On hard error (failed build, dlopen, manifest parse) the loader
     * prints the diagnostic itself; we still continue to a usable
     * pure-Turmeric REPL so the user can debug. The subprocess invokes
     * the `tur` binary on PATH; override via TUR_BIN. */
    const char *no_auto = getenv("TUR_NO_AUTO_SPICE");
    if (!no_auto || strcmp(no_auto, "1") != 0) {
        TurSpiceImage *img = NULL;
        int srv = tur_spice_image_load(".", getenv("TUR_BIN"), &img);
        if (srv == 0 && img) {
            env->spice_image = img;
            /* RP4: register a TuriNativeFn per export so the user can
             * call spice defns directly at the prompt (both bare and
             * `<module>/<defn>` qualified names). The TuriEnv now owns
             * both the image and the binding shims. */
            uint32_t n_exports = tur_spice_image_count(img);
            uint32_t n_bound = tur_ffi_install_spice_bindings(env, img);
            (void)n_bound;
            printf("Loaded spice from %s (%u export%s)\n",
                   tur_spice_image_root(img),
                   n_exports,
                   n_exports == 1 ? "" : "s");
            fflush(stdout);
        }
        /* srv == 1 (no project) and srv == -1 (hard error, already
         * surfaced) both leave env->spice_image NULL: the REPL behaves
         * like a pure-Turmeric session. */
    }

    /* Load history */
    char *hist_path = history_path();
#ifdef TURI_HAVE_EDITLINE
    using_history();
#ifndef _WIN32
    /* wineditline (the libedit MSYS2 ships) implements the core readline API but
     * not stifle_history.  History simply grows unbounded on Windows -- the file
     * is trimmed on read anyway, so this costs memory in a long session, nothing
     * more. */
    stifle_history(1000);
#endif
    if (hist_path) read_history(hist_path);
    /* E11: install tab-completion generator.
     * editline declares rl_completion_entry_function as Function* (int ret)
     * but actual calling convention is char* — suppress the type mismatch. */
    g_completion_env = env;
#  if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wincompatible-function-pointer-types"
#  elif defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
#  endif
    rl_completion_entry_function = tur_completion_generator;
#  if defined(__clang__)
#    pragma clang diagnostic pop
#  elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#  endif
#endif

    Buf multi;
    buf_init(&multi);
    /* Multi-line continuation: brackets the accumulated input leaves open,
     * and whether it ends inside a string / ```c fence / #| |# comment, both
     * from reader_open_depth over the WHOLE buffer (see
     * repl-continuation-counter-misreads-reader-syntax -- a per-line count
     * read a C body's brackets and semicolons as Lisp). */
    int balance = 0;
    bool in_lexeme = false;
    bool in_sweet_form = false; /* 2e: accumulating sweet-exp continuation */

    for (;;) {
        const char *prompt = (multi.len == 0) ? "turmeric> " : "..        ";
        char *line = repl_readline(prompt);

        if (!line) {
            /* EOF (Ctrl-D) */
            if (multi.len > 0) {
                /* Say so: with piped input an unfinished form was dropped
                 * without a word, the same silence the continuation bug
                 * produced for finished ones. */
                multi.len = 0;
                balance = 0;
                in_lexeme = false;
                in_sweet_form = false;
                printf("\n(cancelled)\n");
                continue;
            }
            printf("\n");
            break;
        }

        /* Empty line in multi-line mode:
         * sweet-exp → terminates the form and evaluates;
         * inside a string / ```c fence / #| |# comment → it is content (a
         *   blank line between C statements), so keep it;
         * s-expr    → cancels the incomplete expression. */
        if (line[0] == '\0' && multi.len > 0) {
            if (in_lexeme) {
                buf_putc(&multi, '\n');
                free(line);
                continue;
            }
            if (in_sweet_form) {
                /* Blank line terminates the sweet-exp form */
                in_sweet_form = false;
                balance = 0;
                /* fall through to evaluation below */
                buf_putc(&multi, '\0');
                free(line);
                line = NULL;
                goto repl_do_eval;
            }
            printf("(cancelled)\n");
            multi.len = 0;
            balance = 0;
            in_lexeme = false;
            free(line);
            continue;
        }

        /* Single-line meta-commands (only at the start of an expression) */
        if (multi.len == 0) {
            if (strcmp(line, ":quit") == 0 || strcmp(line, ":q") == 0) {
                free(line);
                break;
            }
            if (strcmp(line, ":help") == 0) {
                print_help();
                free(line);
                continue;
            }
            /* 2d: :reset — clear session and restart with a fresh environment */
            if (strcmp(line, ":reset") == 0) {
                /* r7rs-lang-plan R9: a cleared session keeps its dialect. */
                LangDialect keep_lang = env ? env->lang : LANG_TURMERIC;
                ReaderType  keep_rt   = env ? env->reader_type : READER_TURMERIC;
                turi_env_free(env);
                env = repl_fresh_env(keep_lang, keep_rt);
                balance = 0;
                multi.len = 0;
                in_sweet_form = false;
#ifdef TURI_HAVE_EDITLINE
                g_completion_env = env;
#endif
                printf(";; session cleared\n");
                free(line);
                continue;
            }
            /* :pwd — print the working directory, as the host spells it. */
            if (strcmp(line, ":pwd") == 0) {
                char cwd[PATH_MAX];
                if (repl_logical_cwd(cwd, sizeof cwd)) printf("%s\n", cwd);
                else printf(":pwd failed: %s\n", strerror(errno));
                free(line);
                continue;
            }
            /* :cd [dir] — change the working directory, bare :cd goes home.
             * Unlike a host-side "set directory" this moves the *running*
             * process, so session state survives. Hosts tracking the cwd are
             * told via OSC 7. */
            if (strncmp(line, ":cd", 3) == 0 && (line[3] == ' ' || line[3] == '\0')) {
                const char *arg = (line[3] == ' ') ? line + 4 : "";
                while (*arg == ' ') arg++;
                if (!*arg) {
                    const char *home = getenv("HOME");
                    arg = (home && *home) ? home : "/";
                }
                if (chdir(arg) != 0) {
                    printf(":cd %s: %s\n", arg, strerror(errno));
                } else {
                    char cwd[PATH_MAX];
                    repl_update_logical_pwd(arg);
                    if (repl_logical_cwd(cwd, sizeof cwd)) printf("%s\n", cwd);
                    repl_emit_cwd_marker();
                }
                free(line);
                continue;
            }
            if (strncmp(line, ":type", 5) == 0 && (line[5] == ' ' || line[5] == '\0')) {
                const char *expr = (line[5] == ' ') ? line + 6 : "";
                if (*expr) cmd_type(env, expr);
                else printf(":type requires an expression\n");
                free(line);
                continue;
            }
            if (strncmp(line, ":expand", 7) == 0 && (line[7] == ' ' || line[7] == '\0')) {
                const char *expr = (line[7] == ' ') ? line + 8 : "";
                if (*expr) cmd_expand(env, expr);
                else printf(":expand requires a macro call form\n");
                free(line);
                continue;
            }
            if (strncmp(line, ":doc", 4) == 0 && (line[4] == ' ' || line[4] == '\0')) {
                const char *sym = (line[4] == ' ') ? line + 5 : "";
                if (*sym) cmd_doc(env, sym);
                else printf(":doc requires a symbol name\n");
                free(line);
                continue;
            }
            if (strncmp(line, ":explain", 8) == 0 && (line[8] == ' ' || line[8] == '\0')) {
                const char *arg = (line[8] == ' ') ? line + 9 : "";
                cmd_explain(env, arg);
                free(line);
                continue;
            }
            if (strncmp(line, ":load-string", 12) == 0 &&
                (line[12] == ' ' || line[12] == '\0')) {
                const char *arg = (line[12] == ' ') ? line + 13 : "";
                while (*arg == ' ') arg++;
                if (*arg != '"') {
                    printf(":load-string requires a quoted source string, "
                           "e.g. :load-string \"(defn f [] 1)\\n(f)\"\n");
                    free(line);
                    continue;
                }
                char *src = unquote_literal(&arg);
                if (!src) {
                    printf(":load-string: unterminated string literal\n");
                    free(line);
                    continue;
                }
                /* Route through the normal evaluation path rather than
                 * calling turi_eval directly, so a region gets the same
                 * Show-instance display, `_` binding, and error reporting an
                 * interactively typed form does. */
                multi.len = 0;
                buf_puts(&multi, src);
                buf_putc(&multi, '\0');
                free(src);
                free(line);
                line = NULL;
                balance = 0;
                in_sweet_form = false;
                goto repl_do_eval;
            }
            if (strncmp(line, ":reload", 7) == 0 && (line[7] == ' ' || line[7] == '\0')) {
                const char *path = (line[7] == ' ') ? line + 8 : "";
                if (*path) cmd_reload(env, path);
                else printf(":reload requires a file path\n");
                free(line);
                continue;
            }
            if (strncmp(line, ":run", 4) == 0 && (line[4] == ' ' || line[4] == '\0')) {
                const char *path = (line[4] == ' ') ? line + 5 : "";
                if (*path) {
                    cmd_run(&env, path);
                    if (!env) { free(line); break; }
                    balance = 0;
                    multi.len = 0;
                    in_sweet_form = false;
#ifdef TURI_HAVE_EDITLINE
                    g_completion_env = env;
#endif
                } else {
                    printf(":run requires a file path\n");
                }
                free(line);
                continue;
            }
            if (strcmp(line, ":tutorial") == 0) {
                cmd_tutorial_list();
                free(line);
                continue;
            }
            if (strncmp(line, ":tutorial", 9) == 0 && (line[9] == ' ' || line[9] == '\0')) {
                const char *rest = (line[9] == ' ') ? line + 10 : "";
                if (*rest == '\0') {
                    cmd_tutorial_list();
                } else {
                    /* Parse tutorial name and optional step number */
                    char name[256] = {0};
                    int step = 0;
                    int parsed = sscanf(rest, "%255s %d", name, &step);
                    if (parsed >= 1) {
                        cmd_tutorial_start(name, step);
                    } else {
                        printf(":tutorial requires a tutorial name\n");
                    }
                }
                free(line);
                continue;
            }
            if (strcmp(line, ":tutorial-help") == 0) {
                print_tutorial_help();
                free(line);
                continue;
            }
            if (strcmp(line, ":next") == 0) {
                cmd_tutorial_next();
                free(line);
                continue;
            }
            if (strcmp(line, ":prev") == 0) {
                cmd_tutorial_prev();
                free(line);
                continue;
            }
            if (strcmp(line, ":hint") == 0) {
                cmd_tutorial_hint();
                free(line);
                continue;
            }
            if (strcmp(line, ":skip") == 0) {
                cmd_tutorial_skip();
                free(line);
                continue;
            }
            if (strcmp(line, ":quit-tutorial") == 0) {
                cmd_tutorial_quit();
                free(line);
                continue;
            }
            if (strcmp(line, ":tutorial-progress") == 0) {
                cmd_tutorial_progress();
                free(line);
                continue;
            }
            if (strncmp(line, "#lang ", 6) == 0) {
                const char  *rest    = NULL;
                size_t       rest_len = 0;
                const char  *bad     = NULL;
                size_t       bad_len = 0;
                /* saffron-lang-plan S8: `detect_lang_dialect`, not the plain
                 * `detect_lang`.  The latter reports only the READER
                 * axis, so `#lang saffron` came back as plain `turmeric` with
                 * no dialect, the early-out below fired ("reader already set
                 * to turmeric"), and the language half was silently dropped --
                 * `(defn add [a b] ...)` kept Turmeric's `int` parameter
                 * default in a session the user had just asked to be Saffron.
                 * The guide's first example is that exact line. */
                LangDialect dialect = LANG_TURMERIC;
                ReaderType rt = detect_lang_dialect(line, strlen(line),
                                                    &rest, &rest_len,
                                                    &bad, &bad_len,
                                                    &dialect);
                if (rt == READER_UNKNOWN || rt == (ReaderType)-1) {
                    fprintf(stderr, "unknown #lang: '%s'\n", line + 6);
                } else if (bad) {
                    fprintf(stderr,
                            "#lang takes a single base dialect; unexpected "
                            "trailing token: '%.*s'\n", (int)bad_len, bad);
                } else if (lang_traits(dialect)->prelude != lang_traits(env->lang)->prelude) {
                    /* r7rs-lang-plan R9: the new language has a different
                     * prelude, which the pinned preload does not hold -- a
                     * new session, as :reset builds one (repl_fresh_env). */
                    turi_env_free(env);
                    env = repl_fresh_env(dialect, rt);
                    balance = 0;
                    multi.len = 0;
                    in_sweet_form = false;
#ifdef TURI_HAVE_EDITLINE
                    g_completion_env = env;
#endif
                    if (dialect != LANG_TURMERIC)
                        printf("; language set to %s, reader %s "
                               "(session reset)\n", lang_dialect_name(dialect),
                               reader_type_name(rt));
                    else
                        printf("; reader set to %s (session reset)\n",
                               reader_type_name(rt));
                } else if (rt != env->reader_type || dialect != env->lang) {
                    /* Full switch: rewinds to the pinned stdlib preload
                     * (accumulated USER source may be incompatible with the
                     * new reader, the preload is not) and wipes the session
                     * reader-macro registry, whose user macros came from the
                     * source being discarded. */
                    turi_env_apply_lang_dialect(env, rt, dialect);
                    if (dialect != LANG_TURMERIC)
                        printf("; language set to %s, reader %s "
                               "(session reset)\n", lang_dialect_name(dialect),
                               reader_type_name(rt));
                    else
                        printf("; reader set to %s (session reset)\n",
                               reader_type_name(rt));
                } else {
                    /* "reader already set to <name>" is the pinned wording
                     * (tests/run-flags.sh reader-name-canonical and
                     * lang-same-base-no-reset); the language axis rides
                     * along as a suffix rather than reshaping the line. */
                    printf("; reader already set to %s", reader_type_name(rt));
                    if (dialect != LANG_TURMERIC)
                        printf(" (%s)", lang_dialect_name(dialect));
                    printf("\n");
                }
                free(line);
                continue;
            }
            if (line[0] == ':') {
                printf("unknown meta-command '%s' — try :help\n", line);
                free(line);
                continue;
            }
        }

        /* Accumulate line and update balance. */
        if (line) {
            /* 2e: sweet-exp continuation detection.
             * If in sweet mode and the first token doesn't start with '(',
             * we enter sweet continuation: indented lines continue the form,
             * and a blank line (handled above) terminates it. */
            if ((env->reader_type == READER_SWEET ||
                 env->reader_type == READER_R7RS_SWEET) && multi.len == 0
                    && line[0] != '(' && line[0] != '\0') {
                in_sweet_form = true;
            }

            if (in_sweet_form) {
                /* Inside a sweet-exp form: keep accumulating unconditionally;
                 * termination is via blank line (handled above). */
                if (multi.len > 0) buf_putc(&multi, '\n');
                buf_puts(&multi, line);
                free(line);
                /* Only for the blank-line rule above: a blank line inside a
                 * fence or string is content here too, not the terminator. */
                (void)reader_open_depth(multi.data, multi.len,
                                        env->reader_type, &in_lexeme);
                continue; /* show '..' prompt again */
            }

            if (multi.len > 0) buf_putc(&multi, '\n');
            buf_puts(&multi, line);
            free(line);
            balance = reader_open_depth(multi.data, multi.len,
                                        env->reader_type, &in_lexeme);
        }

        /* If balanced (or over-closed), evaluate */
        if (balance <= 0) {
            balance = 0;

            if (line) buf_putc(&multi, '\0'); /* already NUL-terminated when line==NULL */
            repl_do_eval:;

            /* RP6: --watch -- check freshness right before eval so the
             * source mutation (which typically happens while readline
             * is blocked on the previous prompt) is picked up against
             * the input the user just submitted, not against the next
             * one. The reload helper prints its own "(reload) rebuilt
             * N exports" summary, satisfying the plan's "one-line
             * summary before the next prompt" requirement. */
            if (watch_mode && env->spice_image
                && !tur_spice_image_is_fresh(env->spice_image)) {
                tur_ffi_reload_spice(env);
            }

            /* Check if we're in tutorial mode and this input matches the current step */
            if (g_tutorial_state && g_tutorial_state->in_tutorial) {
                if (check_tutorial_step(env, multi.data)) {
                    multi.len = 0;
                    continue;
                }
            }

            g_last_diag_code[0] = '\0';

            repl_emit_exec_marker();   /* busy from here until the D below */

            char     type_tag[64] = {0};
            TuriValue result = turi_eval_typed(env, multi.data,
                                               type_tag, sizeof(type_tag));

            if (turi_is_error(result)) {
                const char *msg = turi_error_message(result);
                if (msg &&
                    strcmp(msg, "parse error") != 0 &&
                    strcmp(msg, "elaboration error") != 0) {
                    if (use_color)
                        fprintf(stderr, COL_ERR "error: %s" COL_RESET "\n", msg);
                    else
                        fprintf(stderr, "error: %s\n", msg);
                }
            } else {
                /* r7rs-lang-plan R9: an R7RS session echoes in Scheme's own
                 * spelling -- through the prelude's `write`, so a symbol is
                 * `a` rather than `#<struct Sym>` and a list is `(1 2)` --
                 * and prints nothing for the unspecified value, as a Scheme
                 * REPL does after `(display ...)`. */
                bool scheme_echoed = false;
                if (env->lang == LANG_R7RS) {
                    TuriValue wr = turi_env_get(env, "r7rs-write");
                    if (wr.tag == TURI_CLOSURE) {
                        /* r7rs-repl-echoes-multiple-values-opaquely: a turn
                         * that returns any count of values but one returns the
                         * prelude's `R7rsValues` carrier, which `write` has no
                         * case for.  Echo each value on its own `=>` line, as
                         * chibi and Racket do, and nothing for `(values)`.
                         * Nor for a definition, whose value is unspecified.
                         * `_` is the first value. */
                        TuriValue items[64];
                        uint32_t n_items = 0;
                        bool multi = false;
                        TuriValue isv = turi_env_get(env, "r7rs-values?__");
                        TuriValue its = turi_env_get(env, "r7rs-values-items__");
                        TuriValue nul = turi_env_get(env, "r7rs-null?");
                        TuriValue car = turi_env_get(env, "r7rs-car");
                        TuriValue cdr = turi_env_get(env, "r7rs-cdr");
                        if (result.tag != TURI_NIL && !env->last_result_is_def &&
                            isv.tag == TURI_CLOSURE &&
                            its.tag == TURI_CLOSURE && nul.tag == TURI_CLOSURE &&
                            car.tag == TURI_CLOSURE && cdr.tag == TURI_CLOSURE) {
                            TuriValue q = turi_call(env, isv, &result, 1);
                            if (q.tag == TURI_BOOL && q.as_bool) {
                                multi = true;
                                TuriValue l = turi_call(env, its, &result, 1);
                                while (n_items < sizeof items / sizeof items[0] &&
                                       !turi_is_error(l)) {
                                    TuriValue end = turi_call(env, nul, &l, 1);
                                    if (end.tag != TURI_BOOL || end.as_bool) break;
                                    items[n_items++] = turi_call(env, car, &l, 1);
                                    l = turi_call(env, cdr, &l, 1);
                                }
                            }
                        }
                        if (!multi && result.tag != TURI_NIL && !env->last_result_is_def)
                            items[n_items++] = result;
                        for (uint32_t k = 0; k < n_items; k++) {
                            printf("=> ");
                            fflush(stdout);
                            /* `write` is variadic (`[x & port]`), and
                             * turi_call does not pack a rest list: pass the
                             * empty one, so the current output port is used. */
                            TuriValue arg[2] = { items[k], turi_int(0) };   /* nil rest = 0 */
                            TuriValue wres = turi_call(env, wr, arg, 2);
                            if (turi_is_error(wres) && turi_error_message(wres))
                                fprintf(stderr, "write: %s\n", turi_error_message(wres));
                            printf("\n");
                        }
                        if (n_items > 0) turi_env_set(env, "_", items[0]);
                        scheme_echoed = true;
                    }
                }
                /* 2h: for type-level forms that return nil, show a sentinel */
                bool type_sentinel = scheme_echoed;
                if (result.tag == TURI_NIL) {
                    char kind[32] = {0}, name[64] = {0};
                    if (detect_type_form(multi.data, kind, sizeof(kind),
                                         name, sizeof(name))) {
                        if (use_color)
                            printf(COL_NIL "=> #<%s %s>" COL_RESET "\n", kind, name);
                        else
                            printf("=> #<%s %s>\n", kind, name);
                        type_sentinel = true;
                    }
                }
                if (!type_sentinel) {
                    /* SI4: four-tier display:
                     *   1. turi_try_show        -- TURI_STRUCT with Show instance
                     *   2. turi_show_result     -- TURI_INT heap-pointer (Pair, Cons)
                     *   3. turi_try_show_by_tag -- TURI_INT named ADT/struct/coll
                     *                              (Vec, Set, Map, ...) via its Show
                     *   4. repl_print_value     -- default repr */
                    const char *show_str = turi_try_show(env, result);
                    if (!show_str)
                        show_str = turi_show_result(env, result, type_tag);
                    if (!show_str)
                        show_str = turi_try_show_by_tag(env, result, type_tag);
                    if (show_str) {
                        if (use_color)
                            printf("=> " COL_RESET "%s" COL_RESET "\n", show_str);
                        else
                            printf("=> %s\n", show_str);
                        free((char *)show_str);
                    } else {
                        repl_print_value(result, use_color);
                    }
                    /* 2f: bind _ to last non-nil result */
                    if (result.tag != TURI_NIL)
                        turi_env_set(env, "_", result);
                }
            }
            fflush(stdout);

            repl_emit_done_marker(turi_is_error(result) ? 1 : 0);

            multi.len = 0;
        }
    }

    /* Persist history */
#ifdef TURI_HAVE_EDITLINE
    if (hist_path) write_history(hist_path);
#endif
    free(hist_path);

    buf_free(&multi);
    turi_env_free(env);
    return 0;
}
