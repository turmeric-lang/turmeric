/* compiled_turn.c -- C2 step 1 of docs/upcoming/aot-compiled-repl-plan.md.
 * See compiled_turn.h for the model. */
#include "turi/compiled_turn.h"

#include "compiler/expr.h"
#include "compiler/forms.h"
#include "compiler/types.h"
#include "compiler/typeclass.h"
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

/* ---- global slots (step 2) ------------------------------------------------- */

typedef struct {
    char  *name, *typesig;
    size_t size;
    void  *ptr;
} ReplSlot;

static ReplSlot *g_slots;
static uint32_t  g_n_slots, g_cap_slots;
/* The defs the running turn (re)defines: their slots initialize again. */
static char    **g_redefined;
static uint32_t  g_n_redefined;

void *tur_repl_slot(const char *name, const char *typesig, size_t size, int *fresh) {
    bool redefined = false;
    for (uint32_t i = 0; i < g_n_redefined; i++)
        if (strcmp(g_redefined[i], name) == 0) redefined = true;
    for (uint32_t i = 0; i < g_n_slots; i++) {
        ReplSlot *s = &g_slots[i];
        if (strcmp(s->name, name) != 0) continue;
        if (strcmp(s->typesig, typesig) == 0 && s->size == size) {
            *fresh = redefined;
            return s->ptr;
        }
        /* A new type: new storage.  The old stays, for the code of earlier
         * turns that was bound to it. */
        void *p = calloc(1, size ? size : 1);
        char *t = strdup(typesig);
        if (!p || !t) { free(p); free(t); *fresh = 0; return s->ptr; }
        free(s->typesig);
        s->typesig = t;
        s->size = size;
        s->ptr = p;
        *fresh = 1;
        return p;
    }
    if (g_n_slots == g_cap_slots) {
        uint32_t cap = g_cap_slots ? g_cap_slots * 2 : 32;
        ReplSlot *grown = (ReplSlot *)realloc(g_slots, cap * sizeof *grown);
        if (!grown) { fprintf(stderr, "tur: out of memory for a REPL slot\n"); abort(); }
        g_slots = grown;
        g_cap_slots = cap;
    }
    ReplSlot *s = &g_slots[g_n_slots++];
    s->name = strdup(name);
    s->typesig = strdup(typesig);
    s->size = size;
    s->ptr = calloc(1, size ? size : 1);
    if (!s->name || !s->typesig || !s->ptr) {
        fprintf(stderr, "tur: out of memory for a REPL slot\n");
        abort();
    }
    *fresh = 1;
    return s->ptr;
}

static void slots_clear(void) {
    /* The storage itself stays: images of the session that is ending may
     * still name it, and none of them is freed either. */
    for (uint32_t i = 0; i < g_n_slots; i++) {
        free(g_slots[i].name);
        free(g_slots[i].typesig);
    }
    free(g_slots);
    g_slots = NULL;
    g_n_slots = g_cap_slots = 0;
}

void turi_compiled_defs_clear(TuriEnv *env) {
    if (!env) return;
    if (env->compiled_repl) slots_clear();
    for (uint32_t i = 0; i < env->n_compiled_defs; i++) {
        free(env->compiled_defs[i].key);
        free(env->compiled_defs[i].text);
        free(env->compiled_defs[i].type);
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
    "  ```)\n"
    /* The pieces of a structured result (emit_repr): no prefix, no newline. */
    "(defn __tur_repl_raw [s : cstr] : int\n"
    "  ```c\n"
    "  fputs(s, stdout);\n"
    "  return 0;\n"
    "  ```)\n"
    "(defn __tur_repl_raw_int [x : int] : int\n"
    "  ```c\n"
    "  printf(\"%lld\", (long long)x);\n"
    "  return 0;\n"
    "  ```)\n"
    "(defn __tur_repl_raw_float [x : float] : int\n"
    "  ```c\n"
    "  printf(\"%g\", x);\n"
    "  return 0;\n"
    "  ```)\n"
    "(defn __tur_repl_raw_bool [b : bool] : int\n"
    "  ```c\n"
    "  fputs(b ? \"true\" : \"false\", stdout);\n"
    "  return 0;\n"
    "  ```)\n"
    "(defn __tur_repl_raw_qcstr [s : cstr] : int\n"
    "  ```c\n"
    "  printf(\"\\\"%s\\\"\", s ? s : \"\");\n"
    "  return 0;\n"
    "  ```)\n";

/* Show's module, and the printer of a `show` result (an owned String, printed
 * and released), for a program that prints through Show or uses it itself.
 * The interpreter preloads Show; a compiled program loads it like any other
 * stdlib module. */
static const char k_show_load[] = "(load \"stdlib/typeclass-show.tur\")\n";
static const char k_show_printer[] =
    "(defn __tur_repl_raw_string [s : String] : int\n"
    "  (do (__tur_repl_raw (string/to-cstr s)) (string/release s) 0))\n";

static bool type_is_int(TypeKind k) {
    switch (k) {
    case TY_INT: case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
    case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        return true;
    default:
        return false;
    }
}

/* ---- printing a result ---------------------------------------------------

The interpreter's REPL shows a value through its Show instance when it has
one (turi_try_show, turi_try_show_by_tag), and otherwise through its default
repr: `Name { field = value, ... }` for a constructor with fields, `#<struct
Name>` for one without, nested four deep (turi_value_repr_d).  The compiled
turn does the same in the program it builds, from the result's static type:
Show where the session has an instance for the type and for each of its
arguments, the constructor layout for a struct or ADT, the scalar printers
for scalars, and `#<Type>` for anything else. */

typedef struct {
    const TuriEnv *env;
    bool uses_show;
    int tmp;                 /* fresh names: __tur_rN, __tur_repl_vec_N */
    Buf helpers;             /* defns the printer needs, ahead of main */
} ReprCtx;

typedef struct {             /* a generic ADT's parameter -> its argument */
    const char *name;
    const Type *type;
} ReprSub;

static const TypeClass *show_class(const TuriEnv *env) {
    const TypeClassEnv *tc = (const TypeClassEnv *)env->last_tc_env;
    for (const TypeClass *c = tc ? tc->typeclasses : NULL; c; c = c->next)
        if (c->name && strcmp(c->name->name, "Show") == 0) return c;
    return NULL;
}

/* The head constructor's name, for an ADT (possibly applied). */
static const char *head_name(const Type *t) {
    while (t && t->kind == TY_APP && t->as.app.fn) t = t->as.app.fn;
    return t && t->kind == TY_ADT && t->as.adt_.def ? t->as.adt_.def->name : NULL;
}

static bool is_scalar(TypeKind k) {
    return type_is_int(k) || k == TY_FLOAT || k == TY_FLOAT32 || k == TY_FLOAT64 ||
           k == TY_BOOL || k == TY_CSTR;
}

/* True when Show has an instance for t's head (Vec for (Vec Q)). */
static bool has_show_head(const TuriEnv *env, const Type *t);

/* True when `show` on a value of type t resolves: Show has an instance for
 * its head, and for each argument it is applied to. */
static bool has_show(const TuriEnv *env, const Type *t) {
    if (!t) return false;
    if (is_scalar(t->kind)) return true;
    const char *hn = head_name(t);
    const TypeClass *show = show_class(env);
    /* ptr<void>'s instance reads the pointer as a Result carrier; the
     * interpreter's display skips it for any other pointer (show_tag_is_skipped)
     * and so does this. */
    if (!show || t->kind == TY_TYVAR || t->kind == TY_UNKNOWN || t->kind == TY_PTR_VOID) return false;
    const TypeClassEnv *tc = (const TypeClassEnv *)env->last_tc_env;
    bool found = false;
    for (const TypeClassInstance *i = tc->instances; i && !found; i = i->next) {
        if (i->typeclass != show || i->n_type_args < 1) continue;
        const char *in = head_name(&i->type_args[0]);
        /* An ADT matches by its head's name, Vec as (Vec int); a built-in
         * type with an instance of its own (Sym, Char) by its kind. */
        if (hn) found = in && strcmp(in, hn) == 0;
        else found = !in && i->type_args[0].kind == t->kind && t->kind != TY_APP;
    }
    if (!found) return false;
    for (const Type *a = t; a->kind == TY_APP && a->as.app.fn; a = a->as.app.fn)
        if (!has_show(env, a->as.app.arg)) return false;
    return true;
}

static bool has_show_head(const TuriEnv *env, const Type *t) {
    const Type *h = t;
    while (h && h->kind == TY_APP && h->as.app.fn) h = h->as.app.fn;
    return h && has_show(env, h);
}

/* A generic ADT's parameters bound to the arguments t applies it to:
 * (Result int cstr) is APP(APP(Result, int), cstr). */
static uint32_t adt_subst(const Type *t, const AdtDef *def, ReprSub *out, uint32_t cap) {
    const Type *args[16];
    uint32_t n = 0;
    for (const Type *a = t; a->kind == TY_APP && a->as.app.fn && n < 16; a = a->as.app.fn)
        args[n++] = a->as.app.arg;
    uint32_t m = 0;
    for (uint32_t i = 0; i < n && i < def->n_type_params && m < cap; i++)
        out[m++] = (ReprSub){ def->type_params[i], args[n - 1 - i] };
    return m;
}

static void emit_quoted(Buf *b, const char *s) {
    buf_putc(b, '"');
    for (const char *p = s; *p; p++)
        buf_printf(b, (*p == '"' || *p == '\\') ? "\\%c" : "%c", *p);
    buf_putc(b, '"');
}

static void emit_type_sentinel(Buf *b, const Type *t) {
    Buf tn;
    buf_init(&tn);
    if (t) type_print(&tn, *t);
    else buf_puts(&tn, "?");
    buf_putc(&tn, '\0');
    buf_puts(b, "(__tur_repl_raw \"#<");
    for (const char *p = tn.data; *p; p++)
        buf_printf(b, (*p == '"' || *p == '\\') ? "\\%c" : "%c", *p);
    buf_puts(b, ">\")");
    buf_free(&tn);
}

/* An expression printing the value `v` (a variable) of type t, with no
 * prefix and no newline, `depth` levels of constructors deep.  `sub` binds a
 * generic ADT's parameters for its fields' types. */
static void emit_repr(Buf *b, ReprCtx *rc, const char *v, const Type *t, int depth,
                      const ReprSub *sub, uint32_t n_sub) {
    if (t && t->kind == TY_TYVAR) {
        const Type *bound = NULL;
        for (uint32_t i = 0; i < n_sub && !bound; i++)
            if (t->as.tyvar_.name && sub[i].name && strcmp(t->as.tyvar_.name, sub[i].name) == 0)
                bound = sub[i].type;
        t = bound;
    }
    TypeKind k = t ? t->kind : TY_UNKNOWN;
    if (type_is_int(k)) { buf_printf(b, "(__tur_repl_raw_int %s)", v); return; }
    if (k == TY_FLOAT || k == TY_FLOAT32 || k == TY_FLOAT64) {
        buf_printf(b, "(__tur_repl_raw_float %s)", v);
        return;
    }
    if (k == TY_BOOL) { buf_printf(b, "(__tur_repl_raw_bool %s)", v); return; }
    if (k == TY_CSTR) { buf_printf(b, "(__tur_repl_raw_qcstr %s)", v); return; }
    if (k == TY_FN) { buf_puts(b, "(__tur_repl_raw \"#<fn>\")"); return; }
    if (k == TY_NIL) { buf_puts(b, "(__tur_repl_raw \"nil\")"); return; }
    if (has_show(rc->env, t)) {
        rc->uses_show = true;
        buf_printf(b, "(__tur_repl_raw_string (show %s))", v);
        return;
    }
    const Type *head = t;
    while (head && head->kind == TY_APP && head->as.app.fn) head = head->as.app.fn;
    const AdtDef *def = head && head->kind == TY_ADT ? head->as.adt_.def : NULL;
    const char *hn = def ? def->name : NULL;
    if (hn && strcmp(hn, "Vec") == 0 && t->kind == TY_APP && depth > 0) {
        /* A Vec whose elements have no Show: Show [Vec]'s "[a b]" shape, each
         * element printed the way the REPL would print it alone. */
        int id = rc->tmp++;
        Buf vt;
        buf_init(&vt);
        type_print(&vt, *t);
        buf_putc(&vt, '\0');
        Buf elem;   /* a nested Vec adds its own helper while this one is built */
        buf_init(&elem);
        emit_repr(&elem, rc, "__tur_e", t->as.app.arg, depth - 1, sub, n_sub);
        buf_putc(&elem, '\0');
        Buf *h = &rc->helpers;
        buf_printf(h, "(defn __tur_repl_vec_%d [v : %s i : int] : int\n", id, vt.data);
        buf_printf(h, "  (if (< i (vec-len v))\n    (do (when (> i 0) (__tur_repl_raw \" \"))\n"
                      "      (let [__tur_e (vec-get v i)]\n        %s)\n", elem.data);
        buf_printf(h, "      (__tur_repl_vec_%d v (+ i 1)))\n    0))\n", id);
        buf_free(&elem);
        buf_free(&vt);
        buf_printf(b, "(do (__tur_repl_raw \"[\") (__tur_repl_vec_%d %s 0) (__tur_repl_raw \"]\"))",
                   id, v);
        return;
    }
    /* Another container with Show for itself but not for its elements: its
     * fields are its representation, not its contents. */
    if (!def || def->n_ctors == 0 || has_show_head(rc->env, t)) {
        emit_type_sentinel(b, t);
        return;
    }

    ReprSub inner[16];
    uint32_t n_inner = adt_subst(t, def, inner, 16);
    buf_printf(b, "(match %s", v);
    for (uint32_t c = 0; c < def->n_ctors; c++) {
        const CtorDef *cd = def->ctors[c];
        int base = rc->tmp;
        rc->tmp += (int)cd->n_fields;
        buf_printf(b, "\n    (%s", cd->name);
        for (uint32_t f = 0; f < cd->n_fields; f++) buf_printf(b, " __tur_r%d", base + (int)f);
        buf_puts(b, ") ");
        if (cd->n_fields == 0 || depth <= 0) {
            Buf lit;
            buf_init(&lit);
            buf_printf(&lit, "#<struct %s>", cd->name);
            buf_putc(&lit, '\0');
            buf_puts(b, "(__tur_repl_raw ");
            emit_quoted(b, lit.data);
            buf_puts(b, ")");
            buf_free(&lit);
            continue;
        }
        buf_puts(b, "(do ");
        for (uint32_t f = 0; f < cd->n_fields; f++) {
            const CtorField *fd = &cd->fields[f];
            Buf lit;
            buf_init(&lit);
            if (f == 0) buf_printf(&lit, "%s { ", cd->name);
            else buf_puts(&lit, ", ");
            /* A positional field has no name; the value stands alone. */
            if (fd->name) buf_printf(&lit, "%s = ", fd->name);
            buf_putc(&lit, '\0');
            buf_puts(b, "(__tur_repl_raw ");
            emit_quoted(b, lit.data);
            buf_puts(b, ") ");
            buf_free(&lit);
            char var[32];
            snprintf(var, sizeof var, "__tur_r%d", base + (int)f);
            Type scalar;
            const Type *ft = fd->full_type;
            if (!ft) {
                memset(&scalar, 0, sizeof scalar);
                scalar.kind = fd->kind;
                ft = &scalar;
            }
            emit_repr(b, rc, var, ft, depth - 1, inner, n_inner);
            buf_puts(b, " ");
        }
        buf_puts(b, "(__tur_repl_raw \" }\"))");
    }
    buf_puts(b, ")");
}

/* The statement that prints `expr` (its source) of type `t` as the REPL's
 * `=> ...` line. */
static void emit_print(Buf *b, ReprCtx *rc, const char *expr, const Type *t) {
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
        buf_printf(b, "  (let [__tur_v %s]\n    (do (__tur_repl_raw \"=> \")\n      ", expr);
        emit_repr(b, rc, "__tur_v", t, 4, NULL, 0);
        buf_puts(b, "\n      (__tur_repl_raw \"\\n\")))\n");
    }
}

/* True when the program's own text names Show or show, so it needs Show's
 * module the interpreter would have preloaded. */
static bool mentions_show(const char *s) {
    for (const char *p = s; (p = strstr(p, "how")) != NULL; p++)
        if (p > s && (p[-1] == 's' || p[-1] == 'S')) return true;
    return false;
}

/* ---- the turn -------------------------------------------------------------- */

typedef struct {
    char *key, *text, *type;
    bool is_import;
} NewDef;

static void table_put(TuriEnv *env, char *key, char *text, char *type, bool is_import) {
    for (uint32_t i = 0; i < env->n_compiled_defs; i++) {
        if (strcmp(env->compiled_defs[i].key, key) == 0) {
            free(key);
            if (is_import) { free(text); free(type); return; }   /* kept once, in order */
            free(env->compiled_defs[i].text);
            free(env->compiled_defs[i].type);
            env->compiled_defs[i].text = text;       /* replaced in place */
            env->compiled_defs[i].type = type;
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
    env->compiled_defs[env->n_compiled_defs++] = (TuriCompiledDef){ key, text, type, is_import };
}

/* Drop table entry i, keeping the order of the rest. */
static void table_drop(TuriEnv *env, uint32_t i) {
    free(env->compiled_defs[i].key);
    free(env->compiled_defs[i].text);
    free(env->compiled_defs[i].type);
    memmove(&env->compiled_defs[i], &env->compiled_defs[i + 1],
            (env->n_compiled_defs - i - 1) * sizeof env->compiled_defs[0]);
    env->n_compiled_defs--;
}

/* A definer of a type: redefining one can change what a value of it is. */
static bool is_type_definer_key(const char *key) {
    static const char *const heads[] = {
        "defstruct ", "defdata ", "deftype ", "defopaque ", "defgadt ",
        "defrecord ", "defenum ", "defalias ", "defrec ", NULL
    };
    for (int i = 0; heads[i]; i++)
        if (strncmp(key, heads[i], strlen(heads[i])) == 0) return true;
    return false;
}

/* True when the printed type `type` names `name` as a whole token. */
static bool type_mentions(const char *type, const char *name) {
    size_t n = strlen(name);
    if (!type || n == 0) return false;
    static const char delim[] = " ()[]{}<>,:";
    for (const char *p = type; (p = strstr(p, name)) != NULL; p++) {
        bool left = p == type || strchr(delim, p[-1]);
        bool right = p[n] == '\0' || strchr(delim, p[n]);
        if (left && right) return true;
    }
    return false;
}

/* The text of the table with this turn's definitions applied, without
 * changing the table: a turn that fails leaves it as it was. */
static void emit_table(Buf *b, const TuriEnv *env, const NewDef *nd, uint32_t n_nd,
                       const bool *stale, bool imports) {
    for (uint32_t i = 0; i < env->n_compiled_defs; i++) {
        const TuriCompiledDef *d = &env->compiled_defs[i];
        if (d->is_import != imports || (stale && stale[i])) continue;
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
                                 const char *src, Expr *const *items, uint32_t n_items,
                                 const Type *last_type, bool last_is_def) {
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
            nd[n_nd++] = (NewDef){ key, slice(src, forms[i]), NULL, is_import };
            if (!is_import) last_def = forms[i];
        } else {
            exprs[n_exprs++] = slice(src, forms[i]);
        }
    }

    /* A def's value type, as this turn elaborated it. */
    for (uint32_t j = 0; j < n_nd; j++) {
        if (strncmp(nd[j].key, "def ", 4) != 0) continue;
        for (uint32_t k = 0; k < n_items; k++) {
            const Expr *it = items[k];
            if (!it || it->kind != EX_DEF || !it->as.def_.binding || !it->as.def_.init) continue;
            const Binding *bd = it->as.def_.binding;
            if (!bd->name || strcmp(bd->name->name, nd[j].key + 4) != 0) continue;
            Buf tn;
            buf_init(&tn);
            type_print(&tn, it->as.def_.init->type);
            buf_putc(&tn, '\0');
            nd[j].type = tn.data;
        }
    }

    /* A type this turn redefines makes the defs holding its values stale. */
    bool *stale = env->n_compiled_defs ? (bool *)calloc(env->n_compiled_defs, sizeof *stale) : NULL;
    Buf dropped;
    buf_init(&dropped);
    for (uint32_t j = 0; stale && j < n_nd; j++) {
        if (!is_type_definer_key(nd[j].key)) continue;
        bool redefined_type = false;
        for (uint32_t i = 0; i < env->n_compiled_defs; i++)
            if (strcmp(env->compiled_defs[i].key, nd[j].key) == 0 &&
                strcmp(env->compiled_defs[i].text, nd[j].text) != 0)
                redefined_type = true;
        if (!redefined_type) continue;
        const char *tname = strchr(nd[j].key, ' ') + 1;
        size_t mark = dropped.len;
        uint32_t n_dropped = 0;
        for (uint32_t i = 0; i < env->n_compiled_defs; i++) {
            const TuriCompiledDef *d = &env->compiled_defs[i];
            if (stale[i] || strncmp(d->key, "def ", 4) != 0 || !type_mentions(d->type, tname))
                continue;
            bool rewritten = false;   /* this turn defines it again */
            for (uint32_t k = 0; k < n_nd && !rewritten; k++)
                rewritten = strcmp(nd[k].key, d->key) == 0;
            if (rewritten) continue;
            if (n_dropped++ == 0)
                buf_printf(&dropped, "note: %s changed, so the defs holding its old values "
                                     "are dropped; define them again to use them: ", tname);
            stale[i] = true;
            buf_printf(&dropped, "%s%s", n_dropped > 1 ? ", " : "", d->key + 4);
        }
        if (dropped.len > mark) buf_putc(&dropped, '\n');
    }

    /* The program: imports, the printers, the table, then main.  main is built
     * first, since how it prints decides whether Show's module comes in. */
    ReprCtx rc_print = { env, false, 0, { 0 } };
    buf_init(&rc_print.helpers);
    Buf mainb;
    buf_init(&mainb);
    Buf *mb = &mainb;
    bool printed = false;
    for (uint32_t i = 0; i < n_exprs; i++) {
        if (i + 1 == n_exprs && !last_is_def) {
            emit_print(mb, &rc_print, exprs[i], last_type);
            printed = true;
        } else {
            buf_printf(mb, "  %s\n", exprs[i]);
        }
    }
    /* A turn that ends in a definition prints it as the REPL does: a defn as
     * #<fn NAME>, a def as its value.  Type definitions print nothing here;
     * the REPL shows their #<kind Name> sentinel itself. */
    if (!printed && last_is_def && last_def) {
        const char *h = form_head(last_def);
        const char *name = form_def_name(last_def);
        if (name && strcmp(h, "defn") == 0) {
            buf_printf(mb, "  (__tur_repl_put_line \"#<fn %s>\")\n", name);
            printed = true;
        } else if (name && (strcmp(h, "def") == 0 || strcmp(h, "define") == 0)) {
            emit_print(mb, &rc_print, name, last_type);
            printed = true;
        }
    }
    buf_puts(mb, "  0)\n");

    Buf prog;
    buf_init(&prog);
    emit_table(&prog, env, nd, n_nd, stale, /*imports=*/true);
    bool want_show = rc_print.uses_show;
    for (uint32_t i = 0; i < env->n_compiled_defs && !want_show; i++)
        want_show = mentions_show(env->compiled_defs[i].text);
    for (uint32_t j = 0; j < n_nd && !want_show; j++) want_show = mentions_show(nd[j].text);
    for (uint32_t i = 0; i < n_exprs && !want_show; i++) want_show = mentions_show(exprs[i]);
    if (want_show) buf_puts(&prog, k_show_load);
    buf_puts(&prog, k_printers);
    if (rc_print.uses_show) buf_puts(&prog, k_show_printer);
    emit_table(&prog, env, nd, n_nd, stale, /*imports=*/false);
    buf_write(&prog, rc_print.helpers.data, rc_print.helpers.len);
    buf_free(&rc_print.helpers);
    buf_puts(&prog, "(defn main [] : int\n");
    buf_write(&prog, mainb.data, mainb.len);
    buf_free(&mainb);

    /* The defs this turn writes initialize again, in the slot they have. */
    char **redefined = (char **)calloc(n_nd ? n_nd : 1, sizeof *redefined);
    uint32_t n_redefined = 0;
    for (uint32_t j = 0; redefined && j < n_nd; j++)
        if (strncmp(nd[j].key, "def ", 4) == 0) redefined[n_redefined++] = nd[j].key + 4;
    g_redefined = redefined;
    g_n_redefined = n_redefined;

    int rc = 0;
    fflush(stdout);
    int hrc = g_hook->run(prog.data, prog.len, &rc);
    g_redefined = NULL;
    g_n_redefined = 0;
    free(redefined);
    fflush(stdout);
    fflush(stderr);
    buf_free(&prog);

    TuriValue result = turi_nil();
    if (hrc != 0) {
        result = turi_error("the turn did not compile");
    } else if (rc != 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "the turn's program exited with status %d", rc);
        result = turi_error(msg);
    } else {
        for (uint32_t i = env->n_compiled_defs; stale && i-- > 0;)
            if (stale[i]) table_drop(env, i);
        for (uint32_t j = 0; j < n_nd; j++) {
            table_put(env, nd[j].key, nd[j].text, nd[j].type, nd[j].is_import);
            nd[j].key = nd[j].text = nd[j].type = NULL;
        }
        env->last_turn_printed = printed;
        if (dropped.len) {
            fwrite(dropped.data, 1, dropped.len, stderr);
            fflush(stderr);
        }
    }
    free(stale);
    buf_free(&dropped);
    for (uint32_t j = 0; j < n_nd; j++) {
        free(nd[j].key);
        free(nd[j].text);
        free(nd[j].type);
    }
    for (uint32_t i = 0; i < n_exprs; i++) free(exprs[i]);
    free(nd);
    free(exprs);
    return result;
}
