/* compiled_turn.c -- C2 step 1 of docs/upcoming/aot-compiled-repl-plan.md.
 * See compiled_turn.h for the model. */
#include "turi/compiled_turn.h"

#include "compiler/forms.h"
#include "compiler/types.h"
#include "runtime/buf.h"
#include "runtime/experiments.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const TuriCompiledTurnHook *g_hook;

void turi_set_compiled_turn_hook(const TuriCompiledTurnHook *hook) { g_hook = hook; }

bool turi_compiled_turn_available(void) { return g_hook != NULL && g_hook->run != NULL; }

bool turi_compiled_turn_applies(const TuriEnv *env) {
    return env && env->compiled_repl && turi_compiled_turn_available() &&
           env->lang == LANG_TURMERIC && env->reader_type == READER_TURMERIC;
}

void turi_compiled_defs_clear(TuriEnv *env) {
    if (!env) return;
    for (uint32_t i = 0; i < env->n_compiled_defs; i++) {
        free(env->compiled_defs[i].key);
        free(env->compiled_defs[i].text);
    }
    free(env->compiled_defs);
    env->compiled_defs = NULL;
    env->n_compiled_defs = env->cap_compiled_defs = 0;
}

/* ---- the turn's forms ---------------------------------------------------- */

static const char *form_head(const Form *f) {
    if (!f || f->tag != F_LIST || f->as.list.len == 0) return NULL;
    const Form *h = f->as.list.items[0];
    return (h && h->tag == F_SYM && h->as.sym) ? h->as.sym->name : NULL;
}

/* The first symbol after the head that is not an `^attr`: the defined name. */
static const char *form_def_name(const Form *f) {
    for (uint32_t i = 1; i < f->as.list.len; i++) {
        const Form *it = f->as.list.items[i];
        if (it && it->tag == F_SYM && it->as.sym && it->as.sym->name[0] != '^')
            return it->as.sym->name;
    }
    return NULL;
}

static char *slice(const char *src, const Form *f) {
    size_t len = f->span.off_end > f->span.off_start ? f->span.off_end - f->span.off_start : 0;
    char *s = (char *)malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, src + f->span.off_start, len);
    s[len] = '\0';
    return s;
}

static bool is_import_head(const char *h) {
    return strcmp(h, "import") == 0 || strcmp(h, "load") == 0 ||
           strcmp(h, "require") == 0 || strcmp(h, "use") == 0;
}

/* A top-level definer: the table keeps its latest form, keyed by definer and
 * name.  `definstance` is keyed by its class and head instead. */
static bool is_definer_head(const char *h) {
    if (strcmp(h, "define") == 0 || strcmp(h, "extern-c") == 0) return true;
    return strncmp(h, "def", 3) == 0 && strcmp(h, "defmodule") != 0;
}

/* The table key of a definition or import form, malloc'd; NULL for an
 * expression. */
static char *form_key(const Form *f, const char *src, bool *is_import) {
    *is_import = false;
    const char *h = form_head(f);
    if (!h) return NULL;
    Buf k;
    buf_init(&k);
    if (is_import_head(h)) {
        /* Imports are kept in order and deduplicated by their whole text. */
        char *t = slice(src, f);
        buf_printf(&k, "%s", t ? t : "");
        free(t);
        *is_import = true;
    } else if (strcmp(h, "definstance") == 0 && f->as.list.len >= 3) {
        char *cls = slice(src, f->as.list.items[1]);
        char *head = slice(src, f->as.list.items[2]);
        buf_printf(&k, "definstance %s %s", cls ? cls : "", head ? head : "");
        free(cls);
        free(head);
    } else if (is_definer_head(h)) {
        const char *name = form_def_name(f);
        if (!name) { buf_free(&k); return NULL; }
        /* `def` and `define` are one definer. */
        buf_printf(&k, "%s %s", strcmp(h, "define") == 0 ? "def" : h, name);
    } else {
        buf_free(&k);
        return NULL;
    }
    buf_putc(&k, '\0');
    return k.data;
}

/* ---- printing the result ---------------------------------------------------
 *
 * As the REPL prints an interpreter value (turi_value_repr): ints with %lld,
 * floats with %g, a string quoted, a bool as true/false, a unit result as
 * nil.  Through small inline-C printers, so the format is exactly the
 * interpreter's.  Any other type prints as #<Type> -- the interpreter would
 * use the type's Show instance or its struct repr, which step 1 does not
 * reproduce yet (a documented difference). */

static const char k_printers[] =
    "(defn __tur_repl_put_int [x : int] : int\n"
    "  ```c\n"
    "  printf(\"=> %lld\\n\", (long long)x);\n"
    "  return 0;\n"
    "  ```)\n"
    "(defn __tur_repl_put_float [x : float] : int\n"
    "  ```c\n"
    "  printf(\"=> %g\\n\", x);\n"
    "  return 0;\n"
    "  ```)\n"
    "(defn __tur_repl_put_cstr [s : cstr] : int\n"
    "  ```c\n"
    "  printf(\"=> \\\"%s\\\"\\n\", s ? s : \"\");\n"
    "  return 0;\n"
    "  ```)\n"
    "(defn __tur_repl_put_bool [b : bool] : int\n"
    "  ```c\n"
    "  printf(\"=> %s\\n\", b ? \"true\" : \"false\");\n"
    "  return 0;\n"
    "  ```)\n"
    "(defn __tur_repl_put_line [s : cstr] : int\n"
    "  ```c\n"
    "  printf(\"=> %s\\n\", s);\n"
    "  return 0;\n"
    "  ```)\n";

static bool type_is_int(TypeKind k) {
    switch (k) {
    case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
    case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        return true;
    default:
        return false;
    }
}

/* The statement that prints `expr` (its source) of type `t`. */
static void emit_print(Buf *b, const char *expr, const Type *t) {
    TypeKind k = t ? t->kind : TY_NIL;
    if (type_is_int(k)) {
        buf_printf(b, "  (__tur_repl_put_int %s)\n", expr);
    } else if (k == TY_FLOAT || k == TY_FLOAT32 || k == TY_FLOAT64) {
        buf_printf(b, "  (__tur_repl_put_float %s)\n", expr);
    } else if (k == TY_BOOL) {
        buf_printf(b, "  (__tur_repl_put_bool %s)\n", expr);
    } else if (k == TY_CSTR) {
        buf_printf(b, "  (__tur_repl_put_cstr %s)\n", expr);
    } else if (k == TY_FN) {
        /* The interpreter names the closure's FnDef, an internal name for a
         * lambda; compiled code has no such name to give. */
        buf_printf(b, "  %s\n  (__tur_repl_put_line \"#<fn>\")\n", expr);
    } else if (k == TY_NIL || k == TY_NEVER) {
        buf_printf(b, "  %s\n  (__tur_repl_put_line \"nil\")\n", expr);
    } else {
        Buf tn;
        buf_init(&tn);
        type_print(&tn, *t);
        buf_putc(&tn, '\0');
        buf_printf(b, "  %s\n  (__tur_repl_put_line \"#<", expr);
        for (const char *p = tn.data; *p; p++)
            buf_printf(b, (*p == '"' || *p == '\\') ? "\\%c" : "%c", *p);
        buf_puts(b, ">\")\n");
        buf_free(&tn);
    }
}

/* ---- the turn -------------------------------------------------------------- */

typedef struct {
    char *key, *text;
    bool is_import;
} NewDef;

static void table_put(TuriEnv *env, char *key, char *text, bool is_import) {
    for (uint32_t i = 0; i < env->n_compiled_defs; i++) {
        if (strcmp(env->compiled_defs[i].key, key) == 0) {
            free(key);
            if (is_import) { free(text); return; }   /* kept once, in order */
            free(env->compiled_defs[i].text);
            env->compiled_defs[i].text = text;       /* replaced in place */
            return;
        }
    }
    if (env->n_compiled_defs == env->cap_compiled_defs) {
        uint32_t cap = env->cap_compiled_defs ? env->cap_compiled_defs * 2 : 16;
        TuriCompiledDef *grown =
            (TuriCompiledDef *)realloc(env->compiled_defs, cap * sizeof *grown);
        if (!grown) { free(key); free(text); return; }
        env->compiled_defs = grown;
        env->cap_compiled_defs = cap;
    }
    env->compiled_defs[env->n_compiled_defs++] = (TuriCompiledDef){ key, text, is_import };
}

/* The text of the table with this turn's definitions applied, without
 * changing the table: a turn that fails leaves it as it was. */
static void emit_table(Buf *b, const TuriEnv *env, const NewDef *nd, uint32_t n_nd,
                       bool imports) {
    for (uint32_t i = 0; i < env->n_compiled_defs; i++) {
        const TuriCompiledDef *d = &env->compiled_defs[i];
        if (d->is_import != imports) continue;
        const char *text = d->text;
        for (uint32_t j = 0; j < n_nd; j++)
            if (!nd[j].is_import && strcmp(nd[j].key, d->key) == 0) text = nd[j].text;
        buf_printf(b, "%s\n", text);
    }
    for (uint32_t j = 0; j < n_nd; j++) {
        if (nd[j].is_import != imports) continue;
        bool known = false;
        for (uint32_t i = 0; i < env->n_compiled_defs && !known; i++)
            known = strcmp(env->compiled_defs[i].key, nd[j].key) == 0;
        for (uint32_t i = 0; i < j && !known; i++)
            known = strcmp(nd[i].key, nd[j].key) == 0;
        if (!known) buf_printf(b, "%s\n", nd[j].text);
    }
}

TuriValue turi_compiled_turn_run(TuriEnv *env, Form **forms, uint32_t first, uint32_t n,
                                 const char *src, const Type *last_type, bool last_is_def) {
    static bool warned;   /* once per session, not per turn */
    if (!warned) {
        experiment_warn_if_used("compiled-repl");
        warned = true;
    }
    env->last_turn_printed = false;

    uint32_t n_new = n > first ? n - first : 0;
    NewDef *nd = (NewDef *)calloc(n_new ? n_new : 1, sizeof *nd);
    char **exprs = (char **)calloc(n_new ? n_new : 1, sizeof *exprs);
    if (!nd || !exprs) {
        free(nd);
        free(exprs);
        return turi_error("out of memory");
    }
    uint32_t n_nd = 0, n_exprs = 0;
    const Form *last_def = NULL;
    for (uint32_t i = first; i < n; i++) {
        bool is_import = false;
        char *key = form_key(forms[i], src, &is_import);
        if (key) {
            nd[n_nd++] = (NewDef){ key, slice(src, forms[i]), is_import };
            if (!is_import) last_def = forms[i];
        } else {
            exprs[n_exprs++] = slice(src, forms[i]);
        }
    }

    /* The program: imports, the printers, the table, then main. */
    Buf prog;
    buf_init(&prog);
    emit_table(&prog, env, nd, n_nd, /*imports=*/true);
    buf_puts(&prog, k_printers);
    emit_table(&prog, env, nd, n_nd, /*imports=*/false);
    buf_puts(&prog, "(defn main [] : int\n");
    bool printed = false;
    for (uint32_t i = 0; i < n_exprs; i++) {
        if (i + 1 == n_exprs && !last_is_def) {
            emit_print(&prog, exprs[i], last_type);
            printed = true;
        } else {
            buf_printf(&prog, "  %s\n", exprs[i]);
        }
    }
    /* A turn that ends in a definition prints it as the REPL does: a defn as
     * #<fn NAME>, a def as its value.  Type definitions print nothing here;
     * the REPL shows their #<kind Name> sentinel itself. */
    if (!printed && last_is_def && last_def) {
        const char *h = form_head(last_def);
        const char *name = form_def_name(last_def);
        if (name && strcmp(h, "defn") == 0) {
            buf_printf(&prog, "  (__tur_repl_put_line \"#<fn %s>\")\n", name);
            printed = true;
        } else if (name && (strcmp(h, "def") == 0 || strcmp(h, "define") == 0)) {
            emit_print(&prog, name, last_type);
            printed = true;
        }
    }
    buf_puts(&prog, "  0)\n");

    int rc = 0;
    fflush(stdout);
    int hrc = g_hook->run(prog.data, prog.len, &rc);
    fflush(stdout);
    fflush(stderr);
    buf_free(&prog);

    TuriValue result = turi_nil();
    if (hrc != 0) {
        result = turi_error("the turn did not compile");
    } else if (rc != 0) {
        result = turi_error("the turn's program exited non-zero");
    } else {
        for (uint32_t j = 0; j < n_nd; j++) {
            table_put(env, nd[j].key, nd[j].text, nd[j].is_import);
            nd[j].key = nd[j].text = NULL;
        }
        env->last_turn_printed = printed;
    }
    for (uint32_t j = 0; j < n_nd; j++) {
        free(nd[j].key);
        free(nd[j].text);
    }
    for (uint32_t i = 0; i < n_exprs; i++) free(exprs[i]);
    free(nd);
    free(exprs);
    return result;
}
