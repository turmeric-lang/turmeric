/* r7rs_embed.c -- the evaluator behind R7RS `eval` (r7rs-lang-plan T4).
 * See r7rs_embed.h for the model: one embedded R7RS TuriEnv, datums crossing
 * as `write` text, procedures crossing as ids. */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE   /* setenv, strdup */
#endif
#include "turi/r7rs_embed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/builtins.h"
#include "compiler/diag.h"
#include "compiler/lang_dialects.h"
#include "runtime/experiments.h"
#include "runtime/globals.h"
#include "turi/collections_native.h"
#include "turi/env.h"
#include "turi/eval.h"
#include "turi/interpreter_natives.h"
#include "turi/preload.h"

/* The embedded env and the prelude procedures the bridge calls. */
static TuriEnv       *g_env;
static TuriValue      g_read, g_write, g_cons, g_null;
static TuriValue      g_catch_apply, g_raised_p, g_raised_mk, g_condition, g_host_wrap;
static const char    *g_stdlib_hint;
static TuriR7rsHostFn g_host;

/* Embedded procedures handed to the program, by id. */
static TuriValue *g_procs;
static size_t     g_nprocs, g_capprocs;

/* Host procedures registered in the embedded env, by the program's id (a
 * slot is empty until the id is first pushed). */
static TuriValue *g_hosts;
static bool      *g_host_set;
static size_t     g_caphosts;

/* The pending arguments of the next apply. */
static TuriValue *g_pending;
static size_t     g_npending, g_cappending;
static char      *g_pending_err;

/* The last result. */
static char    *g_result_text;
static int64_t  g_result_id;

/* One argument of a host call, and the host calls in progress, innermost
 * last.  They live here, not in host_native's frame: a continuation the
 * program captures inside a host call is a copy of the C stack, and
 * re-entering it returns through host_native again after the first return
 * already finished the call.  So each call is found by its serial, and its
 * arguments are freed by whichever return finds it first -- a re-entry finds
 * none.  A call a continuation escaped from stays until a call below it
 * returns. */
typedef struct { int kind; char *text; int64_t id; } FrameArg;
typedef struct {
    uint64_t  serial;
    FrameArg *args;
    int       n;
} HostCall;
static HostCall *g_calls;
static size_t    g_ncalls, g_capcalls;
static uint64_t  g_call_serial;

/* The answer of the host call that just ended.  Answering is the host
 * function's last act, and host_native takes it as soon as the host function
 * returns, so it never needs to say which call it belongs to -- which a
 * re-entered call could not. */
static int      g_answer_kind = 'U';
static char    *g_answer_text;
static int64_t  g_answer_id;

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { fprintf(stderr, "tur: out of memory (r7rs eval)\n"); abort(); }
    return q;
}

static void set_result_text(const char *s) {
    free(g_result_text);
    g_result_text = strdup(s ? s : "");
}

/* The message of a failed call or eval. */
static const char *failure_message(TuriValue v) {
    if (v.tag == TURI_ERROR && v.as_error) return v.as_error;
    return "evaluation failed";
}
static bool failed(TuriValue v) { return v.tag == TURI_ERROR || v.tag == TURI_THROW; }

/* ---- the bracket around every crossing -------------------------------------
 *
 * Two envs in one process share the elaborator's process-global state, and
 * two pieces of it are keyed to whichever env elaborated LAST: the builtin
 * operator table's `name_sym` pointers (builtins_init stamps them against an
 * env's own symbol table at every elaboration entry, and the tree-walker's
 * dynamic operators look them up at RUN time), and the diagnostic file
 * registry.  Under `tur --interpret` the program's env is one of the two, so
 * every crossing swaps them -- the same bracket src/turi/macro_env.c puts
 * around the macro env's nested evaluations.  A compiled program has no
 * elaborator of its own; the swap is then a harmless no-op on its side. */
static TuriEnv *g_host_env;   /* the interpreter running the program, or NULL */
typedef struct { const SourceFile **files; size_t n; bool had; bool valid; } Side;
static Side g_side_host, g_side_embed;
static bool g_in_embed;

static void side_take(Side *s) {
    size_t cap = diag_files_capacity();
    if (!s->files) s->files = (const SourceFile **)calloc(cap ? cap : 1, sizeof(SourceFile *));
    s->n     = diag_files_save(s->files, cap);
    s->had   = diag_had_error();
    s->valid = true;
}

static void side_put(const Side *s) {
    diag_files_replace(s->files, s->n);
    if (s->had) diag_force_had_error();
}

static void switch_side(bool to_embed) {
    if (g_in_embed == to_embed) return;
    side_take(to_embed ? &g_side_host : &g_side_embed);
    Side *to = to_embed ? &g_side_embed : &g_side_host;
    if (to->valid) side_put(to);
    TuriEnv *env = to_embed ? g_env : g_host_env;
    if (env) builtins_init(&env->st);
    g_in_embed = to_embed;
}

/* The prelude's `r7rs-` names resolve against the session language, so every
 * turn on the embedded env runs with the R7RS prelude as the language
 * prelude; the interpreter host has the same value already, and a compiled
 * program has none of its own. */
static TuriValue embed_eval_text(const char *src) {
    bool was = g_in_embed;
    switch_side(true);
    const char *saved = g_lang_prelude;
    g_lang_prelude = lang_traits(LANG_R7RS)->prelude;
    TuriValue v = turi_eval(g_env, src);
    g_lang_prelude = saved;
    switch_side(was);
    return v;
}

static TuriValue embed_call(TuriValue fn, TuriValue *args, uint32_t n) {
    bool was = g_in_embed;
    switch_side(true);
    const char *saved = g_lang_prelude;
    g_lang_prelude = lang_traits(LANG_R7RS)->prelude;
    TuriValue v = turi_call(g_env, fn, args, n);
    g_lang_prelude = saved;
    switch_side(was);
    return v;
}

/* Create the embedded env on first use: the REPL's R7RS session setup
 * (src/turi/repl.c repl_fresh_env), plus (scheme read), whose reader the
 * bridge uses to read a datum back. */
static bool embed_env(void) {
    if (g_env) return true;
    const char *root = getenv("TUR_STDLIB_DIR");
    if (!root || !*root) {
        root = g_stdlib_hint;
        /* The stdlib's own `(load "stdlib/...")` forms resolve through
         * TUR_STDLIB_DIR (elab_toplevel.c, reader.c), which `tur` exports for
         * itself; a built program does the same with the root it was built
         * against. */
        if (root && *root) {
#ifdef _WIN32
            _putenv_s("TUR_STDLIB_DIR", root);
#else
            setenv("TUR_STDLIB_DIR", root, 1);
#endif
        }
    }
    if (!root || !*root) root = "stdlib";
    bool was = g_in_embed;
    switch_side(true);
    TuriEnv *env = turi_env_new();
    if (!env) { switch_side(was); return false; }
    env->lang        = LANG_R7RS;
    env->reader_type = READER_R7RS;
    const char *saved = g_lang_prelude;
    g_lang_prelude = lang_traits(LANG_R7RS)->prelude;
    turi_env_preload_macros(env, root);
    turi_env_preload_native_stubs(env);
    turi_env_preload_collections(env, root);
    turi_env_preload_typeclasses(env, root);
    turi_env_pin_prelude(env);
    turi_env_register_interpreter_natives(env);
    turi_register_collection_natives(env);
    g_env = env;
    TuriValue v = turi_eval(env, "(import (scheme read))");
    g_lang_prelude = saved;
    switch_side(was);
    if (failed(v)) {
        fprintf(stderr, "tur: eval: the embedded R7RS evaluator did not start (stdlib at '%s')\n", root);
        return false;
    }
    g_read        = turi_env_get(env, "r7rs-bridge-read__");
    g_write       = turi_env_get(env, "r7rs-bridge-write__");
    g_cons        = turi_env_get(env, "r7rs-cons");
    g_null        = turi_env_get(env, "r7rs-null-value__");
    g_catch_apply = turi_env_get(env, "r7rs-bridge-catch-apply__");
    g_raised_p    = turi_env_get(env, "r7rs-bridge-raised?__");
    g_raised_mk   = turi_env_get(env, "r7rs-bridge-raised__");
    g_condition   = turi_env_get(env, "r7rs-bridge-condition__");
    g_host_wrap   = turi_env_get(env, "r7rs-bridge-host-wrap__");
    if (failed(g_read) || failed(g_write) || failed(g_cons) || failed(g_null) || failed(g_catch_apply) ||
        failed(g_raised_p) || failed(g_raised_mk) || failed(g_condition) || failed(g_host_wrap)) {
        fprintf(stderr, "tur: eval: the embedded R7RS prelude is missing its bridge procedures\n");
        return false;
    }
    return true;
}

void turi_r7rs_embed_init_program(const char *stdlib_root, TuriR7rsHostFn host) {
    static bool inited;
    if (!inited) {
        inited = true;
        turi_init(false);
        /* The compile that built this program warned about the experimental
         * language; the embedded elaboration of it does not warn again. */
        experiment_mark_warned("r7rs");
    }
    if (stdlib_root && *stdlib_root) g_stdlib_hint = stdlib_root;
    g_host = host;
}

void turi_r7rs_embed_set_host(TuriR7rsHostFn host, void *host_env) {
    g_host     = host;
    g_host_env = (TuriEnv *)host_env;
}

/* ---- values: embedded -> program ------------------------------------------ */

static bool is_procedure(TuriValue v) { return v.tag == TURI_CLOSURE; }

/* The host id of a registered host native, or -1. */
static int64_t host_id_of(TuriValue v) {
    if (v.tag != TURI_CLOSURE) return -1;
    for (size_t i = 0; i < g_caphosts; i++)
        if (g_host_set[i] && g_hosts[i].as_closure == v.as_closure) return (int64_t)i;
    return -1;
}

static int64_t proc_id_of(TuriValue v) {
    for (size_t i = 0; i < g_nprocs; i++)
        if (g_procs[i].as_closure == v.as_closure) return (int64_t)i;
    if (g_nprocs == g_capprocs) {
        g_capprocs = g_capprocs ? g_capprocs * 2 : 8;
        g_procs = (TuriValue *)xrealloc(g_procs, g_capprocs * sizeof(TuriValue));
    }
    g_procs[g_nprocs] = v;
    return (int64_t)g_nprocs++;
}

/* Classify an embedded value into (kind, text, id). */
static int export_value(TuriValue v, char **text, int64_t *id) {
    *text = NULL;
    *id   = 0;
    if (failed(v)) { *text = strdup(failure_message(v)); return 'E'; }
    if (v.tag == TURI_NIL) return 'U';
    if (v.tag == TURI_STRUCT) {
        TuriValue is = embed_call(g_raised_p, &v, 1);
        if (is.tag == TURI_BOOL && is.as_bool) {
            TuriValue c = embed_call(g_condition, &v, 1);
            if (failed(c) || c.tag != TURI_CSTR || !c.as_cstr) {
                *text = strdup("eval: a raised object does not write as a datum");
                return 'E';
            }
            *text = strdup(c.as_cstr);
            return 'R';
        }
    }
    if (is_procedure(v)) {
        int64_t h = host_id_of(v);
        if (h >= 0) { *id = h; return 'H'; }
        *id = proc_id_of(v);
        return 'P';
    }
    TuriValue s = embed_call(g_write, &v, 1);
    if (failed(s)) { *text = strdup(failure_message(s)); return 'E'; }
    if (s.tag != TURI_CSTR || !s.as_cstr) { *text = strdup("eval: a result did not write as a datum"); return 'E'; }
    *text = strdup(s.as_cstr);
    return 'D';
}

static int set_result(TuriValue v) {
    char   *text;
    int64_t id;
    int kind = export_value(v, &text, &id);
    free(g_result_text);
    g_result_text = text ? text : strdup("");
    g_result_id   = id;
    return kind;
}

/* ---- values: program -> embedded ----------------------------------------- */

static TuriValue read_datum(const char *text) {
    TuriValue arg = turi_cstr(text);
    TuriValue v = embed_call(g_read, &arg, 1);
    if (failed(v)) return turi_errorf("eval: the datum `%s` does not read back: %s", text, failure_message(v));
    return v;
}

static TuriValue host_native(TuriEnv *env, TuriValue *args, uint32_t n, void *ud);

static TuriValue host_value(int64_t host_id) {
    if (host_id < 0) return turi_error("eval: a bad host procedure id");
    size_t h = (size_t)host_id;
    if (h >= g_caphosts) {
        size_t cap = g_caphosts ? g_caphosts : 8;
        while (cap <= h) cap *= 2;
        g_hosts    = (TuriValue *)xrealloc(g_hosts, cap * sizeof(TuriValue));
        g_host_set = (bool *)xrealloc(g_host_set, cap * sizeof(bool));
        for (size_t i = g_caphosts; i < cap; i++) g_host_set[i] = false;
        g_caphosts = cap;
    }
    if (!g_host_set[h]) {
        /* The global binding keeps its NAME pointer for the env's lifetime, so
         * the name lives in the env's symbol arena, not on this stack frame: a
         * stack buffer here was read back by a later lookup that probed this
         * binding's slot (ASan stack-buffer-overflow under `tur jit`). */
        char buf[64];
        int len = snprintf(buf, sizeof buf, "r7rs-host-procedure-%lld__", (long long)host_id);
        char *name = (char *)arena_alloc(&g_env->sym_arena, (size_t)len + 1);
        memcpy(name, buf, (size_t)len + 1);
        turi_env_register_native(g_env, name, host_native, (void *)(intptr_t)host_id);
        TuriValue native = turi_env_get(g_env, name);
        TuriValue wrapped = embed_call(g_host_wrap, &native, 1);
        if (failed(wrapped)) return wrapped;
        g_hosts[h]    = wrapped;
        g_host_set[h] = true;
    }
    return g_hosts[h];
}

static TuriValue proc_value(int64_t proc_id) {
    if (proc_id < 0 || (size_t)proc_id >= g_nprocs) return turi_error("eval: a bad procedure id");
    return g_procs[proc_id];
}

/* The embedded value an answer or a pushed argument names. */
static TuriValue import_value(int kind, const char *text, int64_t id) {
    switch (kind) {
    case 'D': return read_datum(text);
    case 'H': return host_value(id);
    case 'P': return proc_value(id);
    case 'U': return turi_nil();
    case 'R': {
        /* A program procedure raised: the host wrapper raises it again. */
        TuriValue t = turi_cstr(turi_val_strdup(g_env, text ? text : ""));
        return embed_call(g_raised_mk, &t, 1);
    }
    default:  return turi_error(text && *text ? text : "eval: a host procedure failed");
    }
}

/* ---- a host call: evaluated code calls a procedure the program passed in -- */

static void free_args(FrameArg *a, int n) {
    for (int i = 0; i < n; i++) free(a[i].text);
    free(a);
}

/* Ends call `serial`, and any a continuation escaped from above it.  False
 * when the call has ended already: this return is a re-entry. */
static bool call_end(uint64_t serial) {
    size_t k = g_ncalls;
    while (k > 0 && g_calls[k - 1].serial != serial) k--;
    if (k == 0) return false;
    for (size_t i = k - 1; i < g_ncalls; i++) free_args(g_calls[i].args, g_calls[i].n);
    g_ncalls = k - 1;
    return true;
}

static void answer_take(int *kind, char **text, int64_t *id) {
    *kind = g_answer_kind;
    *text = g_answer_text;
    *id   = g_answer_id;
    g_answer_kind = 'U';
    g_answer_text = NULL;
    g_answer_id   = 0;
}

static TuriValue host_native(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env;
    int64_t host_id = (int64_t)(intptr_t)ud;
    if (!g_host) return turi_error("eval: no host to call a program procedure");
    FrameArg *fa = (FrameArg *)calloc(n ? n : 1, sizeof(FrameArg));
    for (uint32_t i = 0; i < n; i++) {
        fa[i].kind = export_value(args[i], &fa[i].text, &fa[i].id);
        if (fa[i].kind == 'E' || fa[i].kind == 'U') {
            /* An unspecified argument travels as a datum the reader cannot
             * make, so it is the named error as well. */
            TuriValue e = turi_errorf("eval: argument %u of a program procedure cannot cross: %s", i + 1,
                                      fa[i].kind == 'U' ? "it is the unspecified value" : fa[i].text);
            free_args(fa, (int)i + 1);
            return e;
        }
    }
    if (g_ncalls == g_capcalls) {
        g_capcalls = g_capcalls ? g_capcalls * 2 : 8;
        g_calls = (HostCall *)xrealloc(g_calls, g_capcalls * sizeof(HostCall));
    }
    uint64_t serial = ++g_call_serial;
    g_calls[g_ncalls++] = (HostCall){ serial, fa, (int)n };
    int     kind;
    char   *text;
    int64_t id;
    answer_take(&kind, &text, &id);   /* nothing is pending; drop it if it is */
    free(text);
    bool was = g_in_embed;
    switch_side(false);
    (void)g_host(host_id);
    switch_side(was);
    answer_take(&kind, &text, &id);
    if (!call_end(serial)) {
        /* This call returned once already, and a continuation the program
         * captured inside it brought control back.  The interpreter refuses
         * such a continuation before it restores anything; a compiled
         * program's call/cc cannot ask, and the evaluator's frames under this
         * one were freed at the first return. */
        fprintf(stderr, "tur: eval: a continuation captured inside a call from evaluated code to a program "
                        "procedure was invoked after that call returned; the evaluator cannot resume it\n");
        exit(70);
    }
    TuriValue r = import_value(kind, text, id);
    free(text);
    return r;
}

uint64_t turi_r7rs_embed_call_serial(void) { return g_ncalls ? g_calls[g_ncalls - 1].serial : 0; }
int turi_r7rs_embed_call_live(uint64_t serial) {
    for (size_t i = g_ncalls; i-- > 0;)
        if (g_calls[i].serial == serial) return 1;
    return 0;
}

/* The innermost host call's arguments: the host function reads them first. */
static const HostCall *call_top(void) { return g_ncalls ? &g_calls[g_ncalls - 1] : NULL; }
int turi_r7rs_embed_frame_count(void) { return call_top() ? call_top()->n : 0; }
int turi_r7rs_embed_frame_kind(int i) {
    const HostCall *c = call_top();
    return c && i >= 0 && i < c->n ? c->args[i].kind : 'U';
}
const char *turi_r7rs_embed_frame_text(int i) {
    const HostCall *c = call_top();
    return c && i >= 0 && i < c->n && c->args[i].text ? c->args[i].text : "";
}
int64_t turi_r7rs_embed_frame_id(int i) {
    const HostCall *c = call_top();
    return c && i >= 0 && i < c->n ? c->args[i].id : 0;
}

static void answer(int kind, const char *text, int64_t id) {
    free(g_answer_text);
    g_answer_kind = kind;
    g_answer_text = text ? strdup(text) : NULL;
    g_answer_id   = id;
}
void turi_r7rs_embed_answer_datum(const char *text) { answer('D', text, 0); }
void turi_r7rs_embed_answer_host(int64_t host_id) { answer('H', NULL, host_id); }
void turi_r7rs_embed_answer_proc(int64_t proc_id) { answer('P', NULL, proc_id); }
void turi_r7rs_embed_answer_unspecified(void) { answer('U', NULL, 0); }
void turi_r7rs_embed_answer_error(const char *msg) { answer('E', msg, 0); }
void turi_r7rs_embed_answer_raise(const char *condition) { answer('R', condition, 0); }

/* ---- the program calls in -------------------------------------------------- */

static void push(TuriValue v) {
    if (g_npending == g_cappending) {
        g_cappending = g_cappending ? g_cappending * 2 : 8;
        g_pending = (TuriValue *)xrealloc(g_pending, g_cappending * sizeof(TuriValue));
    }
    g_pending[g_npending++] = v;
}

static void push_import(int kind, const char *text, int64_t id) {
    if (!embed_env()) return;
    TuriValue v = import_value(kind, text, id);
    if (failed(v) && !g_pending_err) g_pending_err = strdup(failure_message(v));
    push(v);
}
void turi_r7rs_embed_push_datum(const char *text) { push_import('D', text, 0); }
void turi_r7rs_embed_push_host(int64_t host_id) { push_import('H', NULL, host_id); }
void turi_r7rs_embed_push_proc(int64_t proc_id) { push_import('P', NULL, proc_id); }

/* The `(import ...)` form a written list of import sets asks for.  The
 * evaluator libraries themselves are left out: evaluated code has no `eval`
 * of its own (it would re-enter this env), and (scheme r5rs) contributes
 * nothing else a resident library does not. */
static char *import_form(const char *sets_text) {
    const char *drop[] = { "(scheme eval)", "(scheme repl)", "(scheme load)", "(scheme r5rs)" };
    size_t n = strlen(sets_text);
    static const char head[] = "(import (scheme base) ";
    char *out = (char *)malloc(n + sizeof head + 1);
    char *w = out;
    w += sprintf(w, "%s", head);
    const char *p = sets_text;
    if (*p == '(') p++;
    while (*p) {
        bool skipped = false;
        for (size_t i = 0; i < sizeof drop / sizeof drop[0]; i++) {
            size_t dl = strlen(drop[i]);
            if (strncmp(p, drop[i], dl) == 0) { p += dl; skipped = true; break; }
        }
        if (!skipped) *w++ = *p++;
    }
    *w = '\0';
    /* The list's own close paren closes the import form. */
    return out;
}

static int eval_source(const char *sets_text, const char *body) {
    if (!embed_env()) { set_result_text("eval: the embedded R7RS evaluator did not start"); return 'E'; }
    char *imports = import_form(sets_text);
    size_t n = strlen(imports) + strlen(body) + 2;
    char *src = (char *)malloc(n);
    snprintf(src, n, "%s\n%s", imports, body);
    free(imports);
    TuriValue v = embed_eval_text(src);
    free(src);
    return set_result(v);
}

/* A definition is evaluated as it stands; an expression goes through
 * r7rs-bridge-value__, so its value arrives widened to `any` (a top-level
 * expression's value is otherwise its elaborated type's representation). */
static bool is_definition_text(const char *t) {
    return strncmp(t, "(define", 7) == 0 || strncmp(t, "(begin", 6) == 0;
}

int turi_r7rs_embed_eval(const char *sets_text, const char *expr_text) {
    if (is_definition_text(expr_text)) {
        int kind = eval_source(sets_text, expr_text);
        /* A definition's value is unspecified (R7RS 5.3). */
        if (kind != 'E' && strncmp(expr_text, "(define", 7) == 0) kind = 'U';
        return kind;
    }
    size_t n = strlen(expr_text) + 72;
    char *wrapped = (char *)malloc(n);
    snprintf(wrapped, n, "(r7rs-bridge-catch__ (lambda () (r7rs-bridge-value__ %s)))", expr_text);
    int kind = eval_source(sets_text, wrapped);
    free(wrapped);
    return kind;
}

int turi_r7rs_embed_load(const char *sets_text, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        size_t n = strlen(path) + 48;
        char *msg = (char *)malloc(n);
        snprintf(msg, n, "load: cannot open the file %s", path);
        set_result_text(msg);
        free(msg);
        return 'E';
    }
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    size_t got;
    while ((got = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += got;
        if (cap - len < 2) { cap *= 2; buf = (char *)xrealloc(buf, cap); }
    }
    fclose(f);
    buf[len] = '\0';
    /* A `#lang r7rs` line is the session's language already. */
    const char *body = buf;
    if (strncmp(body, "#lang", 5) == 0) {
        const char *nl = strchr(body, '\n');
        body = nl ? nl + 1 : body + len;
    }
    int kind = eval_source(sets_text, body);
    free(buf);
    if (kind != 'E') { free(g_result_text); g_result_text = strdup(""); kind = 'U'; }
    return kind;
}

int turi_r7rs_embed_apply(int64_t proc_id) {
    char *err = g_pending_err;
    g_pending_err = NULL;
    if (!embed_env()) { g_npending = 0; free(err); set_result_text("eval: the embedded R7RS evaluator did not start"); return 'E'; }
    if (err) {
        g_npending = 0;
        set_result_text(err);
        free(err);
        return 'E';
    }
    /* The list is built while the arguments are still in g_pending, where
     * the embedded env's pool sweep reads them (turi_r7rs_embed_roots): a
     * sweep may run inside any embed_call.  Consing calls no host procedure,
     * so nothing pushes meanwhile; the call itself may re-enter, so the
     * arguments are taken before it. */
    TuriValue l = g_null;
    for (size_t i = g_npending; i-- > 0;) {
        TuriValue pair[2] = { g_pending[i], l };
        l = embed_call(g_cons, pair, 2);
        if (failed(l)) break;
    }
    g_npending = 0;
    if (failed(l)) return set_result(l);
    TuriValue call[2] = { proc_value(proc_id), l };
    if (failed(call[0])) return set_result(call[0]);
    return set_result(embed_call(g_catch_apply, call, 2));
}

void turi_r7rs_embed_roots(const void *env, TuriR7rsRootFn fn, void *ud) {
    if (!g_env || env != (const void *)g_env) return;
    /* The prelude procedures are the env's globals as well; held here, they
     * are kept even if evaluated code rebinds a name. */
    const TuriValue fixed[] = { g_read, g_write, g_cons, g_null, g_catch_apply,
                                g_raised_p, g_raised_mk, g_condition, g_host_wrap };
    fn(fixed, sizeof fixed, ud);
    if (g_nprocs) fn(g_procs, g_nprocs * sizeof *g_procs, ud);
    for (size_t i = 0; i < g_caphosts; i++)
        if (g_host_set[i]) fn(&g_hosts[i], sizeof g_hosts[i], ud);
    if (g_npending) fn(g_pending, g_npending * sizeof *g_pending, ud);
}

const char *turi_r7rs_embed_result_text(void) { return g_result_text ? g_result_text : ""; }
int64_t     turi_r7rs_embed_result_id(void) { return g_result_id; }
