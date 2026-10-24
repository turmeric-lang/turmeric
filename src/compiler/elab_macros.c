/* elab_macros.c -- compile-time evaluation, quasiquote, macro definition and expansion. */
#include "elab_internal.h"
#include "prof.h"   /* --timings: the macro-expand zone */
#include "runtime/experiments.h" /* quote-list-data-plan: experiment_warn_if_used */

/* ---- file-local helper forward declarations ---- */
static CtValue ct_value_form(Form *f);
static CtValue ct_value_fn(CtFn *fn);
static CtEnv *ct_env_new(Elab *e, CtEnv *parent, bool *ok);
static void ct_env_bind(CtEnv *env, const Symbol *name, CtValue v);
static bool ct_env_lookup(CtEnv *env, const Symbol *name, CtValue *out);
static bool compile_time_truthy(Form *f);
static Form *ct_value_to_form(CtEnv *env, CtValue v, Span span);
static Form *ct_eval_quasiquote(CtEnv *env, Form *f);
static bool ct_symbol_name(const Symbol *sym, const char *name);
static bool template_needs_ct_eval(Form *f);
static CtValue ct_eval_builtin(CtEnv *env, const Symbol *name, Form **args, uint32_t n_args, Span span);
static CtValue ct_eval_call(CtEnv *env, Form *f);
static CtValue ct_eval_form(CtEnv *env, Form *f);
static CtValue ct_eval_splice_inner(CtEnv *env, Form *f);
static Form *elab_eval_macro_form(Elab *e, Form *f, MacroDef *macro, Form **args);
static Form *substitute_params(Elab *e, Form *f, MacroDef *macro, Form **args, bool mark);

static CtValue ct_value_form(Form *f) {
    CtValue v;
    v.tag = CT_VAL_FORM;
    v.as.form = f;
    return v;
}

static CtValue ct_value_fn(CtFn *fn) {
    CtValue v;
    v.tag = CT_VAL_FN;
    v.as.fn = fn;
    return v;
}

/* Macro arguments are data to the compile-time evaluator.
 *
 * A plain (non-^syntax) parameter is SUBSTITUTED into the template before the
 * template is evaluated, so without a mark the caller's code was evaluated
 * along with the template's own: in `cond`, the clause list
 * `(stopped "s" (= rc 0) "zero" ...)` read as a call to `stopped` and its
 * argument `(= rc 0)` went through the CT `=` builtin -- a Form comparison of
 * the symbol `rc` with `0`, i.e. `false` -- so the expansion tested
 * `(if false ...)`.  A `when` body calling a user function named `second`
 * made the whole template CT-evaluated, and CT `do` dropped the body.
 * cond-bare-symbol-test-folds-later-equality has the cases.
 *
 * So substitute_params wraps every argument it substitutes in an F_UNQUOTE
 * stamped PROV_MACRO_ARG: ct_eval_form already hands a bare unquote's inner
 * back unevaluated, ct_eval_quasiquote and template_needs_ct_eval learn the
 * stamp below, and strip_macro_arg_data removes whatever wrappers survive
 * before the expansion leaves elab_expand_macro.  Builtins still see the
 * argument as a value -- `(first clauses)` is the first clause -- they just
 * never evaluate inside it.  (^syntax parameters were the per-macro fix for
 * this, bound in the CT env instead of substituted; do-m, hamt-of and set-of
 * each needed it.) */
static Form *macro_arg_datum(Elab *e, Form *arg, bool mark) {
    if (!mark || !arg) return arg;
    Form *d = form_unquote(e->arena, arg->span, arg);
    d->fx_prov = (uint8_t)PROV_MACRO_ARG;
    return d;
}

static bool is_macro_arg_datum(const Form *f) {
    return f && f->tag == F_UNQUOTE && f->fx_prov == (uint8_t)PROV_MACRO_ARG &&
           f->as.list.len == 1;
}

/* The argument itself when `f` is a PROV_MACRO_ARG wrapper, else `f`. */
static Form *macro_arg_unwrap(Form *f) {
    return is_macro_arg_datum(f) ? f->as.list.items[0] : f;
}

/* Copy-on-write removal of every PROV_MACRO_ARG wrapper in `f`.  An argument
 * never carries a wrapper of its own (every expansion strips its result), so
 * the walk does not descend into one it unwraps. */
static Form *strip_macro_arg_data(Elab *e, Form *f) {
    if (!f) return f;
    if (is_macro_arg_datum(f)) return f->as.list.items[0];
    switch (f->tag) {
        case F_LIST: case F_VEC: case F_MAP: case F_SET:
        case F_MAP_LITERAL: case F_SET_LITERAL: case F_ROW_LITERAL:
        case F_QUOTE: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING:
        case F_TYPE_ANN: case F_CONTRACT_TYPE: case F_READER_COND: case F_RANGE_VAR:
            break;
        default:
            return f;
    }
    uint32_t n = f->as.list.len;
    Form **items = NULL;
    for (uint32_t i = 0; i < n; i++) {
        Form *it = strip_macro_arg_data(e, f->as.list.items[i]);
        if (it != f->as.list.items[i] && !items) {
            items = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
            for (uint32_t j = 0; j < i; j++) items[j] = f->as.list.items[j];
        }
        if (items) items[i] = it;
    }
    if (!items) return f;
    Form *out = form_new(e->arena, f->tag, f->span);
    out->lit_suffix = f->lit_suffix;
    out->fx_prov    = f->fx_prov;
    out->as.list.items = items;
    out->as.list.len   = n;
    return out;
}

static CtEnv *ct_env_new(Elab *e, CtEnv *parent, bool *ok) {
    CtEnv *env = (CtEnv *)arena_alloc(e->arena, sizeof(CtEnv));
    env->parent = parent;
    env->bindings = NULL;
    env->elab = e;
    env->ok = ok;
    env->expand_macro_head = false;
    return env;
}

/* Evaluate the inner of a `~@` splice. Same as ct_eval_form except a list
 * whose head names a user macro is expanded at compile time (see CtEnv's
 * expand_macro_head field). Used everywhere a splice element is evaluated. */
static CtValue ct_eval_splice_inner(CtEnv *env, Form *f) {
    CtEnv *senv = ct_env_new(env->elab, env, env->ok);
    senv->expand_macro_head = true;
    return ct_eval_form(senv, f);
}

static void ct_env_bind(CtEnv *env, const Symbol *name, CtValue v) {
    CtBinding *b = (CtBinding *)arena_alloc(env->elab->arena, sizeof(CtBinding));
    b->name = name;
    b->value = v;
    b->next = env->bindings;
    env->bindings = b;
}

static bool ct_env_lookup(CtEnv *env, const Symbol *name, CtValue *out) {
    for (CtEnv *cur = env; cur; cur = cur->parent) {
        for (CtBinding *b = cur->bindings; b; b = b->next) {
            if (b->name == name) {
                *out = b->value;
                return true;
            }
        }
    }
    return false;
}

static bool compile_time_truthy(Form *f) {
    return !(f->tag == F_NIL || (f->tag == F_BOOL && !f->as.b));
}

/* Structural form equality now lives in forms.c as form_equal (shared with
 * the interpreter's TURI_SYNTAX `=`); this file's `=` builtin calls it. */

static Form *ct_value_to_form(CtEnv *env, CtValue v, Span span) {
    if (v.tag == CT_VAL_FORM) return v.as.form;
    *env->ok = false;
    diag_emit(DIAG_ERROR, span, "compile-time macro evaluation expected a form value");
    return form_nil(env->elab->arena, span);
}

/* Evaluate the children of a sequence form (list/vec/map/set/...) under
 * quasiquote, expanding any `~@` (F_UNQUOTE_SPLICING) child by splicing the
 * elements of its (list-valued) result into the output.  Shared by every
 * sequence kind so `~@` works uniformly inside `[...]`, `#map{...}`,
 * `#set{...}`, etc. -- not just inside `(...)` lists.
 * docs/archive/history/quasiquote-splice-into-vector-unsupported.md. Returns the
 * expanded item array and writes its length to *out_n; returns NULL (with
 * *env->ok cleared) on a splice-evaluation error. */
static Form **ct_qq_eval_seq_items(CtEnv *env, Form *f, uint32_t *out_n) {
    uint32_t n_in = f->as.list.len;
    Form **tmp = (Form **)arena_alloc(env->elab->arena, n_in * sizeof(Form *));
    bool *splice = (bool *)arena_alloc(env->elab->arena, n_in * sizeof(bool));
    uint32_t n_out = 0;
    for (uint32_t i = 0; i < n_in; i++) {
        Form *it = f->as.list.items[i];
        if (it->tag == F_UNQUOTE_SPLICING) {
            CtValue v = ct_eval_splice_inner(env, it->as.list.items[0]);
            Form *inner = ct_value_to_form(env, v, it->span);
            if (!*env->ok) return NULL;
            tmp[i] = inner;
            if (inner->tag == F_LIST) {
                splice[i] = true;
                n_out += inner->as.list.len;
            } else {
                splice[i] = false;
                n_out++;
            }
        } else {
            tmp[i] = ct_eval_quasiquote(env, it);
            splice[i] = false;
            n_out++;
        }
    }
    Form **items = (Form **)arena_alloc(env->elab->arena, n_out * sizeof(Form *));
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < n_in; i++) {
        if (splice[i]) {
            Form *lst = tmp[i];
            for (uint32_t j = 0; j < lst->as.list.len; j++) items[out_idx++] = lst->as.list.items[j];
        } else {
            items[out_idx++] = tmp[i];
        }
    }
    *out_n = n_out;
    return items;
}

static Form *ct_eval_quasiquote(CtEnv *env, Form *f) {
    /* A substituted argument (`~param` drops its unquote, a bare `param`
     * inside the template is substituted too) is literal data here. */
    if (is_macro_arg_datum(f)) return f->as.list.items[0];
    switch (f->tag) {
        case F_UNQUOTE: {
            CtValue v = ct_eval_form(env, f->as.list.items[0]);
            return ct_value_to_form(env, v, f->span);
        }
        case F_UNQUOTE_SPLICING: {
            CtValue v = ct_eval_splice_inner(env, f->as.list.items[0]);
            return ct_value_to_form(env, v, f->span);
        }
        case F_LIST: {
            uint32_t n_out = 0;
            Form **items = ct_qq_eval_seq_items(env, f, &n_out);
            if (!items) return form_nil(env->elab->arena, f->span);
            /* Gap D: if the constructed list has an inline-C block at its
             * head, the elaborator would otherwise try to elaborate it as
             * a call (with the CBLOCK in callee position, which has type
             * `nil` -- producing the misleading "expression in call head
             * has type `nil`, which is not callable" diagnostic). Auto-
             * wrap with `do` so the CBLOCK sits in a body position and
             * elaborates as a statement, matching Clojure's "leaves self-
             * evaluate in quasiquote" model. Filed and resolved under
             * docs/archive/history/macro-cannot-emit-inline-c-block.md. */
            if (n_out > 0 && items[0]->tag == F_CBLOCK) {
                Form **wrapped = (Form **)arena_alloc(env->elab->arena,
                                                      (n_out + 1) * sizeof(Form *));
                wrapped[0] = form_sym(env->elab->arena, f->span, env->elab->sym_do);
                for (uint32_t i = 0; i < n_out; i++) wrapped[i + 1] = items[i];
                return form_list(env->elab->arena, f->span, wrapped, n_out + 1);
            }
            Form *out = form_list(env->elab->arena, f->span, items, n_out);
            /* Preserve annotation provenance through the template copy: a
             * `#reads w` in a macro template reads as `(reads w)` stamped
             * PROV_READS, and elab_defn's signature walk keys on the stamp
             * -- without it the reconstructed annotation elaborates as a
             * body call ("unknown function or operator 'reads'"). */
            out->fx_prov = f->fx_prov;
            return out;
        }
        case F_VEC:
        case F_MAP:
        case F_SET:
        case F_MAP_LITERAL:
        case F_SET_LITERAL:
        case F_ROW_LITERAL: {
            /* quasiquote-splice-into-vector-unsupported: expand `~@` children
             * here too (previously a 1:1 map that left a splice node as a
             * single bogus element), so a computed field list can be spliced
             * into a `defstruct` field vector, `#map{...}`, `#set{...}`, etc. */
            uint32_t n_out = 0;
            Form **items = ct_qq_eval_seq_items(env, f, &n_out);
            if (!items) return form_nil(env->elab->arena, f->span);
            if (f->tag == F_MAP) {
                Form *m = form_map(env->elab->arena, f->span, items, n_out);
                m->fx_prov = f->fx_prov;   /* keep #fx{...} explicit-row provenance */
                return m;
            }
            if (f->tag == F_SET) return form_set(env->elab->arena, f->span, items, n_out);
            if (f->tag == F_MAP_LITERAL) return form_map_literal(env->elab->arena, f->span, items, n_out);
            if (f->tag == F_SET_LITERAL) return form_set_literal(env->elab->arena, f->span, items, n_out);
            if (f->tag == F_ROW_LITERAL) return form_row_literal(env->elab->arena, f->span, items, n_out);
            return form_vec(env->elab->arena, f->span, items, n_out);
        }
        case F_QUASIQUOTE:
            return ct_eval_quasiquote(env, f->as.list.items[0]);
        case F_CONTRACT_TYPE: {
            /* `#refine{ x : T | (~pred w x) }` in a template: the contract
             * type's predicate slot may hold computed unquotes (e.g. a
             * str->sym-built measure name).  Returning the form as-is left
             * the F_UNQUOTE node in the predicate, which elaborates as an
             * unbound symbol.  Recurse per item and rebuild. */
            uint32_t n = f->as.list.len;
            Form **items = (Form **)arena_alloc(env->elab->arena, n * sizeof(Form *));
            for (uint32_t i = 0; i < n; i++)
                items[i] = ct_eval_quasiquote(env, f->as.list.items[i]);
            return form_contract_type(env->elab->arena, f->span, items, n);
        }
        case F_TYPE_ANN: {
            /* docs/archive/history/macro-template-type-position-rejects-unquoted-compound.md:
             * recurse into the type-annotation payload so unquoted compound
             * expressions inside a `: T` annotation (e.g. `: (Box ~(first xs))`)
             * are evaluated at expansion time rather than left as raw F_UNQUOTE
             * nodes for type_expr_from_form to reject. */
            if (f->as.list.len == 0) return f;
            Form *inner = ct_eval_quasiquote(env, f->as.list.items[0]);
            return form_type_ann(env->elab->arena, f->span, inner);
        }
        default:
            return f;
    }
}

static bool ct_symbol_name(const Symbol *sym, const char *name) {
    size_t n = strlen(name);
    return sym->len == n && strncmp(sym->name, name, n) == 0;
}

/* Single source of truth for the compile-time builtin name set.
 *
 * X(ID, "name", gates): one row per builtin.  `ct_eval_builtin` dispatches
 * off this table (via ct_builtin_kind), and `template_needs_ct_eval` reads
 * the same rows (via ct_builtin_gates), so a builtin can no longer be added
 * to the evaluator and forgotten in the gate -- the historical failure mode
 * where a macro body using the new builtin silently never evaluated.
 *
 * `gates` marks names whose appearance as a call head in a substituted
 * template routes the template through the compile-time evaluator.  Two
 * deliberate exceptions carry gates=false:
 *   - `=` and `not` are recognized builtins but predate the gate, and they
 *     appear in existing templates that are pure runtime code; gating them
 *     would CT-hijack those templates, comparing Forms structurally where
 *     the author meant runtime values.
 *   - `string?` is the same case in advance: it is a natural runtime
 *     predicate name (spice tests call `(assert-true (string? s))`), so it
 *     only evaluates at compile time inside a template something else
 *     already routed there -- as the stdlib `doc` macro's `list` does.
 * `map` is the inverse case: it gates, but is dispatched as a special form
 * in ct_eval_call (the fn argument must stay a CT_VAL_FN), so its switch
 * case below falls through to "not a builtin". */
#define CT_BUILTIN_TABLE(X) \
    X(FIRST,          "first",          true)  \
    X(REST,           "rest",           true)  \
    X(SECOND,         "second",         true)  \
    X(NIL_P,          "nil?",           true)  \
    X(EMPTY_P,        "empty?",         true)  \
    X(LIST_P,         "list?",          true)  \
    X(VEC_P,          "vec?",           true)  \
    X(SYMBOL_NAME,    "symbol-name",    true)  \
    X(TYPE_ANN_P,     "type-ann?",      true)  \
    X(TYPE_ANN_INNER, "type-ann-inner", true)  \
    X(DOT_SYM,        "dot-sym",        true)  \
    X(STR_TO_SYM,     "str->sym",       true)  \
    X(STR_APPEND,     "str-append",     true)  \
    X(CONS,           "cons",           true)  \
    X(LIST,           "list",           true)  \
    X(VEC,            "vec",            true)  \
    X(MAP,            "map",            true)  \
    X(EQ,             "=",              false) \
    X(NOT,            "not",            false) \
    X(STRING_P,       "string?",        false)

typedef enum {
    CT_B_NONE = 0,
#define X(id, name, gates) CT_B_##id,
    CT_BUILTIN_TABLE(X)
#undef X
} CtBuiltinKind;

static const struct { const char *name; bool gates; } CT_BUILTINS[] = {
#define X(id, name, gates) { name, gates },
    CT_BUILTIN_TABLE(X)
#undef X
};

/* Unified gensym: one name-manufacturing path for the three call sites
 * (template substitution, quasiquote expansion outside macro bodies, and
 * expression-position (gensym ...)).
 *
 * Freshness is checked against the symbol table, not just assumed from the
 * counter: every symbol the reader has seen -- the whole current file, the
 * stdlib, and every module loaded so far -- is already interned, so skipping
 * interned candidates means a hand-written `tmp_0` can no longer be captured
 * by (gensym "tmp").  The winning name is interned on return, so later
 * gensyms skip it too. */
static const Symbol *gensym_fresh(Elab *e, const char *prefix) {
    char name_buf[128];
    for (;;) {
        snprintf(name_buf, sizeof(name_buf), "%s_%u", prefix, e->next_gensym_id++);
        StrSlice s = strslice(name_buf, (uint32_t)strlen(name_buf));
        if (!symtab_contains(e->st, s)) return symtab_intern(e->st, s);
    }
}

/* Shared prefix parse for (gensym), (gensym sym), (gensym "str") and
 * (gensym 'sym).  Returns the prefix string, or NULL when the argument is
 * not an acceptable prefix form -- callers decide whether that is an error
 * (expression position) or a silent default (template substitution). */
static const char *gensym_prefix_from_call(const Form *call) {
    if (call->as.list.len < 2) return "g";
    const Form *prefix_f = call->as.list.items[1];
    if (prefix_f->tag == F_SYM) return prefix_f->as.sym->name;
    if (prefix_f->tag == F_STR) return prefix_f->as.s.p;
    if (prefix_f->tag == F_QUOTE && prefix_f->as.list.len == 1 &&
        prefix_f->as.list.items[0]->tag == F_SYM) {
        return prefix_f->as.list.items[0]->as.sym->name;
    }
    return NULL;
}

static CtBuiltinKind ct_builtin_kind(const Symbol *sym) {
    for (size_t i = 0; i < sizeof(CT_BUILTINS) / sizeof(CT_BUILTINS[0]); i++) {
        if (ct_symbol_name(sym, CT_BUILTINS[i].name))
            return (CtBuiltinKind)(i + 1);
    }
    return CT_B_NONE;
}

static bool ct_builtin_gates(const Symbol *sym) {
    CtBuiltinKind k = ct_builtin_kind(sym);
    return k != CT_B_NONE && CT_BUILTINS[k - 1].gates;
}

/* Decide whether a substituted macro template must run through the
 * compile-time evaluator (elab_eval_macro_form) before elaboration.
 * True when the template still carries quasiquote machinery, or calls a
 * gate-listed CT builtin (see CT_BUILTIN_TABLE above).  Surviving orphan
 * F_UNQUOTE nodes recurse into their inner form only: both this evaluator
 * and the elaborator's quasiquote path merely unwrap an unquote outside
 * quasiquote, so gating on the wrapper alone would not change its meaning
 * but WOULD expose bare do/let/if heads elsewhere in the template to CT
 * special-form hijacking. */
static bool template_needs_ct_eval(Form *f) {
    switch (f->tag) {
        case F_LIST:
            if (f->as.list.len > 0 && f->as.list.items[0]->tag == F_SYM &&
                ct_builtin_gates(f->as.list.items[0]->as.sym)) {
                return true;
            }
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (template_needs_ct_eval(f->as.list.items[i])) return true;
            }
            return false;
        case F_VEC:
        case F_MAP:
        case F_SET:
        case F_MAP_LITERAL:
        case F_SET_LITERAL:
        case F_ROW_LITERAL:
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (template_needs_ct_eval(f->as.list.items[i])) return true;
            }
            return false;
        case F_TYPE_ANN:
            /* docs/archive/history/macro-template-type-position-rejects-unquoted-compound.md:
             * a `: (T ~(first xs))` annotation wraps the type-expression in an
             * F_TYPE_ANN whose payload still carries the unquote node.  If we
             * report `false` here, elab_eval_macro_form is skipped and the raw
             * F_UNQUOTE reaches type_expr_from_form, which rejects it.  Recurse
             * into the payload so a CT-evaluable form in type position triggers
             * the post-substitute ct_eval_quasiquote pass. */
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (template_needs_ct_eval(f->as.list.items[i])) return true;
            }
            return false;
        case F_UNQUOTE:
            /* The caller's argument: what it contains says nothing about
             * the template, so it cannot route the template through the
             * CT evaluator (a `when` body calling `second`). */
            if (is_macro_arg_datum(f)) return false;
            return template_needs_ct_eval(f->as.list.items[0]);
        case F_UNQUOTE_SPLICING:
            return template_needs_ct_eval(f->as.list.items[0]);
        case F_QUASIQUOTE:
            /* substitute_params now preserves the quasiquote wrapper to
             * shield runtime do/let/if heads from CT special-form
             * dispatch.  We must still run ct_eval to unwrap it. */
            return true;
        case F_QUOTE:
        case F_SYM:
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_CBLOCK:
        /* CT0: Contract type annotations don't contain CT builtins at top level */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals don't contain CT builtins */
        case F_READER_COND:
        /* RR3: Range literal variable annotation doesn't contain CT builtins */
        case F_RANGE_VAR:
            return false;
    }
    return false;
}

static CtValue ct_eval_builtin(CtEnv *env, const Symbol *name, Form **args, uint32_t n_args, Span span) {
    switch (ct_builtin_kind(name)) {
    case CT_B_FIRST: {
        if (n_args != 1) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, span, "compile-time first expects 1 argument");
            return ct_value_form(form_nil(env->elab->arena, span));
        }
        if ((args[0]->tag != F_LIST && args[0]->tag != F_VEC) || args[0]->as.list.len == 0) return ct_value_form(form_nil(env->elab->arena, span));
        return ct_value_form(args[0]->as.list.items[0]);
    }
    case CT_B_REST: {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time rest expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        if ((args[0]->tag != F_LIST && args[0]->tag != F_VEC) || args[0]->as.list.len <= 1) return ct_value_form(form_list(env->elab->arena, span, NULL, 0));
        uint32_t len = args[0]->as.list.len - 1;
        Form **items = (Form **)arena_alloc(env->elab->arena, len * sizeof(Form *));
        for (uint32_t i = 0; i < len; i++) items[i] = args[0]->as.list.items[i + 1];
        return ct_value_form(form_list(env->elab->arena, span, items, len));
    }
    case CT_B_SECOND: {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time second expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        if ((args[0]->tag != F_LIST && args[0]->tag != F_VEC) || args[0]->as.list.len < 2) return ct_value_form(form_nil(env->elab->arena, span));
        return ct_value_form(args[0]->as.list.items[1]);
    }
    case CT_B_NIL_P: {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time nil? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, args[0]->tag == F_NIL));
    }
    case CT_B_EMPTY_P: {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time empty? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        bool empty = args[0]->tag == F_NIL ||
                     ((args[0]->tag == F_LIST || args[0]->tag == F_VEC || args[0]->tag == F_MAP || args[0]->tag == F_SET) && args[0]->as.list.len == 0);
        return ct_value_form(form_bool(env->elab->arena, span, empty));
    }
    case CT_B_LIST_P: {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time list? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, args[0]->tag == F_LIST));
    }
    case CT_B_VEC_P: {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time vec? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, args[0]->tag == F_VEC));
    }
    case CT_B_STRING_P: {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time string? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, args[0]->tag == F_STR));
    }
    case CT_B_SYMBOL_NAME: {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time symbol-name expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        /* A quoted symbol names the same symbol: `'x` reads as an F_QUOTE
         * node and `(quote x)` as a list, and a macro taking a name (the
         * stdlib `doc`) should not have to tell the three spellings apart. */
        Form *sf = args[0];
        if (sf->tag == F_QUOTE && sf->as.list.len == 1)
            sf = sf->as.list.items[0];
        else if (sf->tag == F_LIST && sf->as.list.len == 2 &&
                 sf->as.list.items[0]->tag == F_SYM &&
                 sf->as.list.items[0]->as.sym == env->elab->sym_quote)
            sf = sf->as.list.items[1];
        if (sf->tag != F_SYM) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time symbol-name expects a symbol"); return ct_value_form(form_nil(env->elab->arena, span)); }
        const Symbol *sym = sf->as.sym;
        return ct_value_form(form_str(env->elab->arena, span, sym->name, sym->len));
    }
    case CT_B_TYPE_ANN_P: {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time type-ann? expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, args[0]->tag == F_TYPE_ANN));
    }
    case CT_B_TYPE_ANN_INNER: {
        /* Unwrap an F_TYPE_ANN node so a macro can read the type form a
         * structural `name : type` annotation carries. The reader folds
         * `: type` into form_type_ann(inner) (src/compiler/forms.c:142),
         * stored as items[0] with len=1. CT first/rest/second only
         * descend F_LIST/F_VEC, so the macro author has no other way
         * through. See docs/archive/history/ct-primitives-cannot-walk-type-ann-nodes.md. */
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time type-ann-inner expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        if (args[0]->tag != F_TYPE_ANN) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time type-ann-inner expects an annotation form"); return ct_value_form(form_nil(env->elab->arena, span)); }
        return ct_value_form(args[0]->as.list.items[0]);
    }
    case CT_B_DOT_SYM: {
        /* (dot-sym field-sym) => the symbol ".field-sym" for use in method calls */
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time dot-sym expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        if (args[0]->tag != F_SYM) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time dot-sym expects a symbol"); return ct_value_form(form_nil(env->elab->arena, span)); }
        const Symbol *sym = args[0]->as.sym;
        uint32_t new_len = sym->len + 1;
        char *buf = (char *)arena_alloc(env->elab->arena, new_len + 1);
        buf[0] = '.';
        memcpy(buf + 1, sym->name, sym->len);
        buf[new_len] = '\0';
        const Symbol *new_sym = symtab_intern(env->elab->st, strslice(buf, new_len));
        return ct_value_form(form_sym(env->elab->arena, span, new_sym));
    }
    case CT_B_STR_TO_SYM: {
        /* (str->sym s) => intern s as a symbol */
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time str->sym expects 1 argument"); return ct_value_form(form_nil(env->elab->arena, span)); }
        if (args[0]->tag != F_STR) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time str->sym expects a string"); return ct_value_form(form_nil(env->elab->arena, span)); }
        const Symbol *new_sym = symtab_intern(env->elab->st, strslice(args[0]->as.s.p, args[0]->as.s.len));
        return ct_value_form(form_sym(env->elab->arena, span, new_sym));
    }
    case CT_B_STR_APPEND: {
        /* (str-append s1 s2 ...) => compile-time string concatenation */
        size_t total = 0;
        for (uint32_t i = 0; i < n_args; i++) {
            if (args[i]->tag != F_STR) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time str-append expects string arguments"); return ct_value_form(form_nil(env->elab->arena, span)); }
            total += args[i]->as.s.len;
        }
        char *buf = (char *)arena_alloc(env->elab->arena, total + 1);
        size_t off = 0;
        for (uint32_t i = 0; i < n_args; i++) {
            memcpy(buf + off, args[i]->as.s.p, args[i]->as.s.len);
            off += args[i]->as.s.len;
        }
        buf[total] = '\0';
        return ct_value_form(form_str(env->elab->arena, span, buf, (uint32_t)total));
    }
    case CT_B_CONS: {
        if (n_args != 2) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time cons expects 2 arguments"); return ct_value_form(form_nil(env->elab->arena, span)); }
        Form *tail = args[1];
        uint32_t tail_len = 0;
        if (tail->tag == F_LIST) {
            tail_len = tail->as.list.len;
        } else if (tail->tag != F_NIL) {
            tail_len = 1;
        }
        Form **items = (Form **)arena_alloc(env->elab->arena, (tail_len + 1) * sizeof(Form *));
        items[0] = args[0];
        if (tail->tag == F_LIST) {
            for (uint32_t i = 0; i < tail_len; i++) items[i + 1] = tail->as.list.items[i];
        } else if (tail->tag != F_NIL) {
            items[1] = tail;
        }
        return ct_value_form(form_list(env->elab->arena, span, items, tail_len + 1));
    }
    case CT_B_LIST: {
        Form **items = (n_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_args * sizeof(Form *));
        for (uint32_t i = 0; i < n_args; i++) items[i] = args[i];
        return ct_value_form(form_list(env->elab->arena, span, items, n_args));
    }
    case CT_B_VEC: {
        Form **items = (n_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_args * sizeof(Form *));
        for (uint32_t i = 0; i < n_args; i++) items[i] = args[i];
        return ct_value_form(form_vec(env->elab->arena, span, items, n_args));
    }
    case CT_B_EQ: {
        if (n_args != 2) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time = expects 2 arguments"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, form_equal(args[0], args[1])));
    }
    case CT_B_NOT: {
        if (n_args != 1) { *env->ok = false; diag_emit(DIAG_ERROR, span, "compile-time not expects 1 argument"); return ct_value_form(form_bool(env->elab->arena, span, false)); }
        return ct_value_form(form_bool(env->elab->arena, span, !compile_time_truthy(args[0])));
    }
    case CT_B_MAP:  /* CT special form, dispatched in ct_eval_call */
    case CT_B_NONE:
        break;
    }
    return ct_value_form(NULL);
}

static CtValue ct_eval_call(CtEnv *env, Form *f) {
    if (f->as.list.len == 0) return ct_value_form(form_list(env->elab->arena, f->span, NULL, 0));
    Form *head = f->as.list.items[0];
    /* A call the template writes with a caller-supplied head, `(op x)` for a
     * parameter `op`: the head is the caller's data, so it never names a CT
     * special form, builtin or fn -- even when it is spelled `first` -- but
     * the template's own arguments are still evaluated. */
    if (is_macro_arg_datum(head)) {
        uint32_t n_in = f->as.list.len - 1;
        Form **tmp = (n_in == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_in * sizeof(Form *));
        bool *spl = (n_in == 0) ? NULL : (bool *)arena_alloc(env->elab->arena, n_in * sizeof(bool));
        uint32_t n_out = 1;
        for (uint32_t i = 0; i < n_in; i++) {
            Form *a = f->as.list.items[i + 1];
            bool is_spl = a->tag == F_UNQUOTE_SPLICING;
            CtValue v = is_spl ? ct_eval_splice_inner(env, a->as.list.items[0])
                               : ct_eval_form(env, a);
            tmp[i] = ct_value_to_form(env, v, a->span);
            if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            spl[i] = is_spl && tmp[i]->tag == F_LIST;
            n_out += spl[i] ? tmp[i]->as.list.len : 1;
        }
        Form **items = (Form **)arena_alloc(env->elab->arena, n_out * sizeof(Form *));
        uint32_t k = 0;
        items[k++] = head->as.list.items[0];
        for (uint32_t i = 0; i < n_in; i++) {
            if (!spl[i]) { items[k++] = tmp[i]; continue; }
            for (uint32_t j = 0; j < tmp[i]->as.list.len; j++) items[k++] = tmp[i]->as.list.items[j];
        }
        return ct_value_form(form_list(env->elab->arena, f->span, items, n_out));
    }
    /* Non-symbol head: cannot be a CT special form or builtin.
     * Return the form as-is so it can be elaborated later.
     * This preserves unevaluated clause lists inside variadic macros. */
    if (head->tag != F_SYM) {
        return ct_value_form(strip_macro_arg_data(env->elab, f));
    }
    /* At this point head->tag == F_SYM is guaranteed by the early return above. */
    if (head->as.sym == env->elab->sym_quote && f->as.list.len == 2) {
        /* `(quote clauses)` on a parameter is the idiom a macro used to keep
         * its argument as data (ecs's defxsystem walks its clauses this way).
         * The argument IS data now, behind its PROV_MACRO_ARG wrapper; quote
         * yields the argument, not the wrapper -- a walker reading the
         * wrapper saw one element and stopped (or, recursing on `rest`,
         * never did). */
        return ct_value_form(macro_arg_unwrap(f->as.list.items[1]));
    }
    if (head->as.sym == env->elab->sym_quasiquote && f->as.list.len == 2) {
        return ct_value_form(ct_eval_quasiquote(env, f->as.list.items[1]));
    }
    if (head->as.sym == env->elab->sym_if) {
        if (f->as.list.len < 3 || f->as.list.len > 4) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time if requires 2 or 3 arguments");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        CtValue cond_v = ct_eval_form(env, f->as.list.items[1]);
        Form *cond = ct_value_to_form(env, cond_v, f->as.list.items[1]->span);
        if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
        if (cond->tag == F_BOOL || cond->tag == F_NIL) {
            if (compile_time_truthy(cond)) return ct_eval_form(env, f->as.list.items[2]);
            if (f->as.list.len == 4) return ct_eval_form(env, f->as.list.items[3]);
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        Form **items = (Form **)arena_alloc(env->elab->arena, f->as.list.len * sizeof(Form *));
        items[0] = head;
        items[1] = cond;
        CtValue then_v = ct_eval_form(env, f->as.list.items[2]);
        items[2] = ct_value_to_form(env, then_v, f->as.list.items[2]->span);
        if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
        if (f->as.list.len == 4) {
            CtValue else_v = ct_eval_form(env, f->as.list.items[3]);
            items[3] = ct_value_to_form(env, else_v, f->as.list.items[3]->span);
            if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        return ct_value_form(form_list(env->elab->arena, f->span, items, f->as.list.len));
    }
    if (head->as.sym == env->elab->sym_do) {
        uint32_t n = f->as.list.len - 1;
        /* Internal defines: splice before evaluating. */
        if (n > 0) {
            Form *spliced = splice_internal_defines(env->elab,
                                f->as.list.items + 1, n, f->span);
            if (spliced) return ct_eval_form(env, spliced);
        }
        CtValue out = ct_value_form(form_nil(env->elab->arena, f->span));
        for (uint32_t i = 1; i < f->as.list.len; i++) {
            out = ct_eval_form(env, f->as.list.items[i]);
            if (!*env->ok) return out;
        }
        return out;
    }
    if (head->as.sym == env->elab->sym_let) {
        if (f->as.list.len < 3) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time let requires bindings and body");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        Form *bindings = macro_arg_unwrap(f->as.list.items[1]);
        if (bindings->tag != F_VEC && bindings->tag != F_LIST) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, bindings->span, "compile-time let bindings must be a vector or list");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        if ((bindings->as.list.len % 2) != 0) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, bindings->span, "compile-time let requires an even number of binding forms");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        CtEnv *inner = ct_env_new(env->elab, env, env->ok);
        for (uint32_t i = 0; i < bindings->as.list.len; i += 2) {
            Form *name_f = macro_arg_unwrap(bindings->as.list.items[i]);
            if (name_f->tag != F_SYM) {
                *env->ok = false;
                diag_emit(DIAG_ERROR, name_f->span, "compile-time let binding name must be a symbol");
                return ct_value_form(form_nil(env->elab->arena, f->span));
            }
            CtValue init_v = ct_eval_form(inner, bindings->as.list.items[i + 1]);
            if (!*env->ok) return init_v;
            ct_env_bind(inner, name_f->as.sym, init_v);
        }
        uint32_t body_count = f->as.list.len - 2;
        /* Internal defines in let body. */
        if (body_count > 0) {
            Form *spliced = splice_internal_defines(env->elab,
                                f->as.list.items + 2, body_count, f->span);
            if (spliced) return ct_eval_form(inner, spliced);
        }
        CtValue out = ct_value_form(form_nil(env->elab->arena, f->span));
        for (uint32_t i = 2; i < f->as.list.len; i++) {
            out = ct_eval_form(inner, f->as.list.items[i]);
            if (!*env->ok) return out;
        }
        return out;
    }
    /* Compile-time letrec: pre-bind all names, then elaborate inits, then body. */
    if (head->as.sym == env->elab->sym_letrec) {
        if (f->as.list.len < 3) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time letrec requires bindings and body");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        Form *bindings = macro_arg_unwrap(f->as.list.items[1]);
        if (bindings->tag != F_VEC && bindings->tag != F_LIST) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, bindings->span,
                      "compile-time letrec bindings must be a vector");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        if ((bindings->as.list.len % 2) != 0) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, bindings->span,
                      "compile-time letrec requires an even number of binding forms");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        CtEnv *inner = ct_env_new(env->elab, env, env->ok);
        /* Pass A: pre-bind all names to nil so mutual references resolve. */
        Form *nil_form = form_nil(env->elab->arena, f->span);
        for (uint32_t i = 0; i < bindings->as.list.len; i += 2) {
            Form *name_f = macro_arg_unwrap(bindings->as.list.items[i]);
            if (name_f->tag != F_SYM) {
                *env->ok = false;
                diag_emit(DIAG_ERROR, name_f->span,
                          "compile-time letrec binding name must be a symbol");
                return ct_value_form(nil_form);
            }
            ct_env_bind(inner, name_f->as.sym, ct_value_form(nil_form));
        }
        /* Pass B: elaborate inits and rebind. */
        for (uint32_t i = 0; i < bindings->as.list.len; i += 2) {
            Form *name_f = macro_arg_unwrap(bindings->as.list.items[i]);
            CtValue init_v = ct_eval_form(inner, bindings->as.list.items[i + 1]);
            if (!*env->ok) return init_v;
            ct_env_bind(inner, name_f->as.sym, init_v);
        }
        /* Pass C: elaborate body. */
        uint32_t body_count = f->as.list.len - 2;
        if (body_count > 0) {
            Form *spliced = splice_internal_defines(env->elab,
                                f->as.list.items + 2, body_count, f->span);
            if (spliced) return ct_eval_form(inner, spliced);
        }
        CtValue out2 = ct_value_form(form_nil(env->elab->arena, f->span));
        for (uint32_t i = 2; i < f->as.list.len; i++) {
            out2 = ct_eval_form(inner, f->as.list.items[i]);
            if (!*env->ok) return out2;
        }
        return out2;
    }
    if (head->as.sym == env->elab->sym_fn) {
        if (f->as.list.len < 3) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time fn requires params and body");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        Form *params_f = f->as.list.items[1];
        if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
            /* A caller-supplied parameter vector: this lambda is the
             * caller's runtime code threaded through, not a CT fn. */
            if (is_macro_arg_datum(params_f))
                return ct_value_form(strip_macro_arg_data(env->elab, f));
            *env->ok = false;
            diag_emit(DIAG_ERROR, params_f->span, "compile-time fn params must be a vector or list");
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        /* A compile-time fn (the kind applied at expansion time, e.g. inside a
         * compile-time `map`) takes only bare-symbol params and no return-type
         * annotation. A typed inline lambda -- `(fn [x : int] : void ...)` --
         * reaches this evaluator only when it is threaded through as data (an
         * argument value spliced via `~`/`~@`), where it is inert runtime AST
         * that must be spliced verbatim, not validated under compile-time fn
         * rules. Detect that shape (a type-annotated parameter, or a return
         * type annotation) and return the form untouched.
         * docs/archive/history/macro-typed-inline-lambda-arg.md */
        {
            bool runtime_lambda = false;
            for (uint32_t i = 0; i < params_f->as.list.len; i++) {
                if (params_f->as.list.items[i]->tag != F_SYM) { runtime_lambda = true; break; }
            }
            /* `(fn PARAMS : RET BODY...)`: a return-type annotation sits right
             * after the param vector as an F_TYPE_ANN (spaced `: T`) or
             * F_KEYWORD (fused `:T`). A body form is never either of those. */
            if (!runtime_lambda && f->as.list.len >= 4) {
                Form *after = f->as.list.items[2];
                if (after->tag == F_TYPE_ANN || after->tag == F_KEYWORD) runtime_lambda = true;
            }
            if (runtime_lambda) return ct_value_form(strip_macro_arg_data(env->elab, f));
        }
        uint32_t n_total = params_f->as.list.len;
        Form **params = (Form **)arena_alloc(env->elab->arena, n_total * sizeof(Form *));
        uint32_t n_fixed = 0;
        bool is_variadic = false;
        const Symbol *rest_param = NULL;
        for (uint32_t i = 0; i < n_total; i++) {
            Form *p = params_f->as.list.items[i];
            if (p->tag != F_SYM) {
                *env->ok = false;
                diag_emit(DIAG_ERROR, p->span, "compile-time fn parameter must be a symbol");
                return ct_value_form(form_nil(env->elab->arena, f->span));
            }
            if (p->as.sym == env->elab->sym_borrow) {
                if (i + 1 >= n_total) {
                    *env->ok = false;
                    diag_emit(DIAG_ERROR, p->span, "compile-time fn '&' must be followed by a rest parameter");
                    return ct_value_form(form_nil(env->elab->arena, f->span));
                }
                if (i + 2 < n_total) {
                    *env->ok = false;
                    diag_emit(DIAG_ERROR, params_f->as.list.items[i + 2]->span, "compile-time fn allows only one rest parameter");
                    return ct_value_form(form_nil(env->elab->arena, f->span));
                }
                Form *rest_f = params_f->as.list.items[i + 1];
                if (rest_f->tag != F_SYM) {
                    *env->ok = false;
                    diag_emit(DIAG_ERROR, rest_f->span, "compile-time fn rest parameter must be a symbol");
                    return ct_value_form(form_nil(env->elab->arena, f->span));
                }
                is_variadic = true;
                rest_param = rest_f->as.sym;
                break;
            }
            params[n_fixed++] = p;
        }
        Form *body = NULL;
        if (f->as.list.len == 3) {
            body = f->as.list.items[2];
        } else {
            uint32_t n_body = f->as.list.len - 2;
            Form **body_items = (Form **)arena_alloc(env->elab->arena, (n_body + 1) * sizeof(Form *));
            body_items[0] = form_sym(env->elab->arena, f->span, env->elab->sym_do);
            for (uint32_t i = 0; i < n_body; i++) body_items[i + 1] = f->as.list.items[i + 2];
            body = form_list(env->elab->arena, f->span, body_items, n_body + 1);
        }
        CtFn *fn = (CtFn *)arena_alloc(env->elab->arena, sizeof(CtFn));
        fn->params = params;
        fn->n_params = n_fixed;
        fn->is_variadic = is_variadic;
        fn->rest_param = rest_param;
        fn->body = body;
        fn->env = env;
        fn->span = f->span;
        return ct_value_fn(fn);
    }

    /* Compile-time (map fn seq): apply a CT fn value to each element of a
     * list/vec and collect the results into a list. Handled here rather than
     * in ct_eval_builtin because the function argument must stay a CT_VAL_FN
     * instead of being forced through ct_value_to_form (which only accepts
     * data forms). This lets a template GENERATE the sequence it splices, e.g.
     * `(do ~@(map (fn [x] `(println ~x)) items))`.
     * docs/archive/history/ct-macro-evaluator-no-function-call-in-splice.md */
    if (ct_builtin_kind(head->as.sym) == CT_B_MAP && f->as.list.len == 3) {
        CtValue fn_v = ct_eval_form(env, f->as.list.items[1]);
        if (!*env->ok) return fn_v;
        /* Only treat this as the compile-time map when the first argument is a
         * CT fn (e.g. a literal `(fn ...)`) over a sequence. A `(map f xs)`
         * whose `f` is an ordinary runtime function is left to the general
         * path below, which rebuilds it as data -- so a runtime map call sitting
         * in a macro template is not hijacked or rejected here. */
        if (fn_v.tag == CT_VAL_FN && !fn_v.as.fn->is_variadic && fn_v.as.fn->n_params == 1) {
            CtFn *fn = fn_v.as.fn;
            CtValue seq_v = ct_eval_form(env, f->as.list.items[2]);
            Form *seq = ct_value_to_form(env, seq_v, f->as.list.items[2]->span);
            if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            if (seq->tag == F_LIST || seq->tag == F_VEC || seq->tag == F_NIL) {
                uint32_t n = (seq->tag == F_NIL) ? 0 : seq->as.list.len;
                Form **out = (n == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n * sizeof(Form *));
                for (uint32_t i = 0; i < n; i++) {
                    CtEnv *call_env = ct_env_new(env->elab, fn->env, env->ok);
                    ct_env_bind(call_env, fn->params[0]->as.sym, ct_value_form(seq->as.list.items[i]));
                    CtValue r = ct_eval_form(call_env, fn->body);
                    out[i] = ct_value_to_form(env, r, fn->span);
                    if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
                }
                return ct_value_form(form_list(env->elab->arena, f->span, out, n));
            }
        }
        /* fall through: not a CT map -- rebuild as a data/runtime call */
    }

    CtValue head_v = ct_eval_form(env, head);
    if (!*env->ok) return head_v;
    /* A user macro this splice will expand below.  Its arguments are written
     * by the enclosing template -- `~@(fields ~cap ~(rest comps))` -- and a
     * `~x` there means the VALUE of x.  A bare unquote evaluates to its inner
     * UNevaluated (ct_eval_form), which used to be finished by substitution:
     * the inner macro's `(empty? comps)` evaluated the substituted `(rest
     * comps)` in place.  Substituted arguments are data now (PROV_MACRO_ARG),
     * so that never happened and the recursion never reached its base case
     * (ecs's sized-mono-world-fields).  Evaluate it here instead. */
    bool splice_macro_call = env->expand_macro_head && head_v.tag != CT_VAL_FN &&
                             elab_lookup_macro(env->elab, head->as.sym) != NULL;
    uint32_t n_in_args = f->as.list.len - 1;
    Form **tmp_args = (n_in_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_in_args * sizeof(Form *));
    bool *splice = (n_in_args == 0) ? NULL : (bool *)arena_alloc(env->elab->arena, n_in_args * sizeof(bool));
    uint32_t n_args = 0;
    for (uint32_t i = 0; i < n_in_args; i++) {
        Form *arg_form = f->as.list.items[i + 1];
        if (splice_macro_call && arg_form->tag == F_UNQUOTE &&
            !is_macro_arg_datum(arg_form) && arg_form->as.list.len == 1)
            arg_form = arg_form->as.list.items[0];
        if (arg_form->tag == F_UNQUOTE_SPLICING) {
            CtValue v = ct_eval_splice_inner(env, arg_form->as.list.items[0]);
            Form *inner = ct_value_to_form(env, v, arg_form->span);
            if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            tmp_args[i] = inner;
            if (inner->tag == F_LIST) {
                splice[i] = true;
                n_args += inner->as.list.len;
            } else {
                splice[i] = false;
                n_args += 1;
            }
        } else {
            CtValue v = ct_eval_form(env, arg_form);
            Form *evaluated = ct_value_to_form(env, v, arg_form->span);
            if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            tmp_args[i] = evaluated;
            splice[i] = false;
            n_args += 1;
        }
    }
    Form **args = (n_args == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_args * sizeof(Form *));
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < n_in_args; i++) {
        if (splice && splice[i]) {
            Form *lst = tmp_args[i];
            for (uint32_t j = 0; j < lst->as.list.len; j++) args[out_idx++] = lst->as.list.items[j];
        } else if (n_args > 0) {
            args[out_idx++] = tmp_args ? tmp_args[i] : NULL;
        }
    }

    /* head->tag == F_SYM is guaranteed here by the early return above. */
    {
        CtValue b = ct_eval_builtin(env, head->as.sym, args, n_args, f->span);
        if (b.tag == CT_VAL_FORM && b.as.form != NULL) return b;
    }

    if (head_v.tag == CT_VAL_FN) {
        CtFn *fn = head_v.as.fn;
        if (!fn->is_variadic && n_args != fn->n_params) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time fn expected %u arguments, got %u", fn->n_params, n_args);
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        if (fn->is_variadic && n_args < fn->n_params) {
            *env->ok = false;
            diag_emit(DIAG_ERROR, f->span, "compile-time fn expected at least %u arguments, got %u", fn->n_params, n_args);
            return ct_value_form(form_nil(env->elab->arena, f->span));
        }
        CtEnv *call_env = ct_env_new(env->elab, fn->env, env->ok);
        for (uint32_t i = 0; i < fn->n_params; i++) ct_env_bind(call_env, fn->params[i]->as.sym, ct_value_form(args[i]));
        if (fn->is_variadic) {
            uint32_t n_rest = n_args - fn->n_params;
            Form **rest = (n_rest == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, n_rest * sizeof(Form *));
            for (uint32_t i = 0; i < n_rest; i++) rest[i] = args[fn->n_params + i];
            ct_env_bind(call_env, fn->rest_param, ct_value_form(form_list(env->elab->arena, f->span, rest, n_rest)));
        }
        return ct_eval_form(call_env, fn->body);
    }

    /* The head names neither a CT special form, a builtin, nor a bound CT fn.
     * When this call is the direct inner of a `~@` splice (expand_macro_head),
     * resolve it as a user macro and expand it at compile time so a macro
     * buried in a splice -- in particular one that recurses over a list, e.g.
     * `(chain (rest xs))` -- expands instead of leaking through as an unbound
     * symbol once the surrounding template is spliced into place. Outside a
     * splice the call is left as data (rebuilt below) for ordinary
     * elaboration, so macro-built data forms threaded through `list`/`cons`
     * (e.g. an accumulated `(map-assoc ...)`) are not prematurely expanded.
     * docs/archive/history/ct-macro-evaluator-no-function-call-in-splice.md */
    if (env->expand_macro_head) {
        MacroDef *macro = elab_lookup_macro(env->elab, head->as.sym);
        if (macro) {
            Form *expanded = elab_expand_macro(env->elab, macro, args, n_args);
            if (!expanded) {
                *env->ok = false;
                return ct_value_form(form_nil(env->elab->arena, f->span));
            }
            return ct_value_form(expanded);
        }
    }

    Form *head_form = ct_value_to_form(env, head_v, head->span);
    if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
    Form **items = (Form **)arena_alloc(env->elab->arena, (n_args + 1) * sizeof(Form *));
    items[0] = head_form;
    for (uint32_t i = 0; i < n_args; i++) items[i + 1] = args[i];
    return ct_value_form(form_list(env->elab->arena, f->span, items, n_args + 1));
}

static CtValue ct_eval_form(CtEnv *env, Form *f) {
    switch (f->tag) {
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_QUOTE:
        case F_CBLOCK:
            return ct_value_form(f);
        case F_QUASIQUOTE:
            return ct_value_form(ct_eval_quasiquote(env, f->as.list.items[0]));
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            return ct_value_form(f->as.list.items[0]);
        case F_TYPE_ANN:
        /* CT0: Contract type annotations are compile-time literals */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals are compile-time literals */
        case F_READER_COND:
        /* RR3: Range literal variable annotation is a compile-time literal */
        case F_RANGE_VAR:
            /* Type annotations are compile-time literals */
            return ct_value_form(f);
        case F_SYM: {
            CtValue v;
            if (ct_env_lookup(env, f->as.sym, &v)) return v;
            return ct_value_form(f);
        }
        case F_LIST:
            return ct_eval_call(env, f);
        case F_VEC:
        case F_MAP:
        case F_SET:
        case F_MAP_LITERAL:
        case F_SET_LITERAL:
        case F_ROW_LITERAL: {
            Form **items = (f->as.list.len == 0) ? NULL : (Form **)arena_alloc(env->elab->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                CtValue v = ct_eval_form(env, f->as.list.items[i]);
                items[i] = ct_value_to_form(env, v, f->as.list.items[i]->span);
                if (!*env->ok) return ct_value_form(form_nil(env->elab->arena, f->span));
            }
            if (f->tag == F_MAP) return ct_value_form(form_map(env->elab->arena, f->span, items, f->as.list.len));
            if (f->tag == F_SET) return ct_value_form(form_set(env->elab->arena, f->span, items, f->as.list.len));
            if (f->tag == F_MAP_LITERAL) return ct_value_form(form_map_literal(env->elab->arena, f->span, items, f->as.list.len));
            if (f->tag == F_SET_LITERAL) return ct_value_form(form_set_literal(env->elab->arena, f->span, items, f->as.list.len));
            if (f->tag == F_ROW_LITERAL) return ct_value_form(form_row_literal(env->elab->arena, f->span, items, f->as.list.len));
            return ct_value_form(form_vec(env->elab->arena, f->span, items, f->as.list.len));
        }
    }
    return ct_value_form(f);
}

static Form *elab_eval_macro_form(Elab *e, Form *f, MacroDef *macro, Form **args) {
    bool ok = true;
    CtEnv *env = ct_env_new(e, NULL, &ok);
    /* Bind ^syntax params so (first decl) / (symbol-name decl) in the macro
     * body see the raw arg Form as a CT value instead of trying to evaluate
     * the substituted code (which would hit the bug in
     * docs/archive/history/macro-args-elaborated-before-expansion.md). */
    if (macro && macro->is_syntax_param) {
        for (uint32_t i = 0; i < macro->n_params; i++) {
            if (!macro->is_syntax_param[i]) continue;
            Form *param = macro->params[i];
            if (param->tag == F_SYM) {
                ct_env_bind(env, param->as.sym, ct_value_form(args[i]));
            }
        }
    }
    if (macro && macro->rest_is_syntax && macro->rest_param) {
        ct_env_bind(env, macro->rest_param, ct_value_form(args[macro->n_params]));
    }
    CtValue v = ct_eval_form(env, f);
    if (!ok) return NULL;
    if (v.tag != CT_VAL_FORM) {
        diag_emit(DIAG_ERROR, f->span, "compile-time macro body did not evaluate to a form");
        return NULL;
    }
    return v.as.form;
}

/* Phase 6: Register a macro */
void elab_register_macro(Elab *e, MacroDef *macro) {
    if (e->n_macros >= e->cap_macros) {
        e->cap_macros = e->cap_macros ? e->cap_macros * 2 : 8;
        e->macros = (MacroDef **)realloc(e->macros, e->cap_macros * sizeof(MacroDef *));
    }
    e->macros[e->n_macros++] = macro;
}

/* Phase 6: Expand quasiquote forms recursively */
Form *quasiquote_expand_form(Elab *e, Form *f) {
    switch (f->tag) {
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_SYM:
            /* Bug 6 / Option A: literals and symbols inside quasiquote are
             * already literal data in the resulting form; returning them as-is
             * lets the elaborator dispatch special-form symbols like `do`,
             * `let`, `if`, etc.  Wrapping them in (quote x) defeated that
             * dispatch because the call head became an F_QUOTE node, not an
             * F_SYM, and the elaborator would re-elaborate the inner symbol
             * via the unbound-binding lookup path. */
            return f;
        case F_QUOTE:
            /* (quasiquote (quote x)) -> (quote (quasiquote x)) */
            {
                Form *quoted_form = f->as.list.items[0];
                Form *expanded_quoted = quasiquote_expand_form(e, quoted_form);
                return form_quote(e->arena, f->span, expanded_quoted);
            }
        case F_UNQUOTE:
            /* (quasiquote (unquote x)) -> x */
            return f->as.list.items[0];
        case F_UNQUOTE_SPLICING:
            /* (quasiquote (unquote-splicing x)) -> x */
            return f->as.list.items[0];
        case F_LIST: {
            /* Check for gensym call: (gensym ...) */
            if (f->as.list.len >= 1 && f->as.list.items[0]->tag == F_SYM) {
                const Symbol *name = f->as.list.items[0]->as.sym;
                if (name == e->sym_gensym) {
                    /* Generate a fresh symbol for gensym inside quasiquote */
                    const char *prefix = gensym_prefix_from_call(f);
                    return form_sym(e->arena, f->span,
                                    gensym_fresh(e, prefix ? prefix : "g"));
                }
            }
            /* Lists inside quasiquote: process each element */
            /* (quasiquote (a ~b c)) -> (a b c) with b unbound from macro params */
            /* For phase 6, we build a list with expanded elements */
            {
                Form **new_items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
                for (uint32_t i = 0; i < f->as.list.len; i++) {
                    Form *item = f->as.list.items[i];
                    if (item->tag == F_UNQUOTE) {
                        /* Unwrap unquote -- substitute_params will handle the inner. */
                        new_items[i] = item->as.list.items[0];
                    } else if (item->tag == F_UNQUOTE_SPLICING) {
                        /* Bug 6 / Option A: preserve the unquote-splicing wrapper
                         * so substitute_params's splice path can detect it and
                         * splice a substituted list-valued rest parameter into
                         * the parent list. */
                        new_items[i] = item;
                    } else {
                        /* Recursively expand other items */
                        new_items[i] = quasiquote_expand_form(e, item);
                    }
                }
                return form_list(e->arena, f->span, new_items, f->as.list.len);
            }
        }
        case F_VEC:
        case F_MAP:
        case F_SET:
        case F_MAP_LITERAL:
        case F_SET_LITERAL:
        case F_ROW_LITERAL:
            /* Vectors/maps/sets inside quasiquote: process each element */
            {
                Form **new_items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
                for (uint32_t i = 0; i < f->as.list.len; i++) {
                    Form *item = f->as.list.items[i];
                    if (item->tag == F_UNQUOTE || item->tag == F_UNQUOTE_SPLICING) {
                        /* Vectors/maps/sets don't go through substitute_params's
                         * splice path, so unwrap here.  (Splicing into a vec/map
                         * literal isn't well-defined; the existing behavior of
                         * embedding the spliced form is preserved.) */
                        new_items[i] = item->as.list.items[0];
                    } else {
                        new_items[i] = quasiquote_expand_form(e, item);
                    }
                }
                if (f->tag == F_MAP) return form_map(e->arena, f->span, new_items, f->as.list.len);
                if (f->tag == F_SET) return form_set(e->arena, f->span, new_items, f->as.list.len);
                if (f->tag == F_MAP_LITERAL) return form_map_literal(e->arena, f->span, new_items, f->as.list.len);
                if (f->tag == F_SET_LITERAL) return form_set_literal(e->arena, f->span, new_items, f->as.list.len);
                if (f->tag == F_ROW_LITERAL) return form_row_literal(e->arena, f->span, new_items, f->as.list.len);
                return form_vec(e->arena, f->span, new_items, f->as.list.len);
            }
        case F_CBLOCK:
        case F_TYPE_ANN:
        /* CT0: Contract type annotations are passed through in quasiquote */
        case F_CONTRACT_TYPE:
        /* INT-1: Reader conditionals are passed through in quasiquote */
        case F_READER_COND:
        /* RR3: Range literal variable annotation is passed through in quasiquote */
        case F_RANGE_VAR:
            return f;
        case F_QUASIQUOTE:
            /* Expand the quasiquoted form */
            return quasiquote_expand_form(e, f->as.list.items[0]);
    }
    return f;
}

/* A `~@X` whose (substituted) inner X is a compile-time computation -- a CT
 * builtin / `map` call, a user-macro call, or any form containing CT builtins
 * -- must NOT be eagerly flattened as data by substitute_params's splice path.
 * Keep the `~@` wrapper instead so the post-substitution ct_eval_quasiquote
 * pass evaluates X and splices its RESULT. Without this, `~@(map ...)` /
 * `~@(chain ...)` are flattened into their literal call tokens (`map`, the fn,
 * the argument list), which then reach the elaborator as an unbound symbol.
 * docs/archive/history/ct-macro-evaluator-no-function-call-in-splice.md */
static bool splice_inner_needs_ct_eval(Elab *e, Form *f) {
    if (template_needs_ct_eval(f)) return true;
    if (f->tag == F_LIST && f->as.list.len > 0 &&
        f->as.list.items[0]->tag == F_SYM &&
        elab_lookup_macro(e, f->as.list.items[0]->as.sym) != NULL) {
        return true;
    }
    return false;
}

/* Phase 6: Helper to substitute parameters in a form */
static Form *substitute_params(Elab *e, Form *f, MacroDef *macro, Form **args, bool mark) {
    switch (f->tag) {
        case F_NIL:
        case F_BOOL:
        case F_INT:
        case F_FLOAT:
        case F_STR:
        case F_KEYWORD:
        case F_CBLOCK:
        case F_QUOTE:
        /* INT-1: Reader conditionals are returned as-is in macro substitution */
        case F_READER_COND:
        /* RR3: Range literal variable annotation is returned as-is in macro substitution */
        case F_RANGE_VAR:
            /* Literals, quote forms, and type annotations are returned as-is */
            return f;
        case F_CONTRACT_TYPE: {
            /* CT0 originally returned contract types as-is, which left
             * `~param` markers inside a template's `#refine{...}` predicate
             * unsubstituted (unbound-symbol at the call site).  Recurse per
             * item, mirroring the F_TYPE_ANN Gap-G fix below. */
            uint32_t n = f->as.list.len;
            Form **items = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
            /* Unmarked: ct_eval_form hands a contract type back untouched,
             * so a PROV_MACRO_ARG wrapper inside one would never be undone
             * by evaluation (the final strip would, but nothing here needs
             * the protection -- the CT evaluator does not look inside). */
            for (uint32_t i = 0; i < n; i++)
                items[i] = substitute_params(e, f->as.list.items[i], macro, args, false);
            return form_contract_type(e->arena, f->span, items, n);
        }
        case F_TYPE_ANN: {
            /* Gap G: the reader wraps `: type-expr` into an F_TYPE_ANN whose
             * items[0] holds the type-expression sub-form. Returning the
             * wrapper as-is leaves any unquote inside untouched, which then
             * reaches type_expr_from_form and trips the
             * "unsupported type expression form" diagnostic at expansion
             * time. Recurse so `~Symbol` markers inside type-position
             * unquotes get substituted from macro args. Filed and
             * resolved under
             * docs/archive/history/macro-unquote-in-type-position-rejected.md. */
            if (f->as.list.len == 0) return f;
            Form *inner = substitute_params(e, f->as.list.items[0], macro, args, false);
            return form_type_ann(e->arena, f->span, inner);
        }
        case F_QUASIQUOTE:
            /* Preserve the quasiquote wrapper so ct_eval_quasiquote can
             * later treat the inner form as data -- otherwise stripping
             * the wrapper here exposes runtime `do`/`let`/`if` heads to
             * the CT special-form dispatch in ct_eval_call, which would
             * (e.g.) reduce a (do ...) body to just its tail expression
             * and silently drop sibling forms. */
            return form_quasiquote(e->arena, f->span,
                                   substitute_params(e, f->as.list.items[0], macro, args, mark));
        case F_UNQUOTE: {
            /* If the unquote's inner is a bare macro-parameter symbol,
             * inline the arg form directly (dropping the unquote wrapper).
             * This preserves the legacy semantics that `~param` splices
             * the caller-supplied form verbatim -- without it, ct_eval
             * would re-interpret arg forms whose head is `fn`/`do`/`let`
             * as compile-time special forms.  Computed unquotes like
             * `~(dot-sym A)` keep their wrapper so ct_eval_quasiquote
             * can evaluate them. */
            Form *inner = f->as.list.items[0];
            if (inner->tag == F_SYM) {
                for (uint32_t i = 0; i < macro->n_params; i++) {
                    Form *param = macro->params[i];
                    if (param->tag == F_SYM && param->as.sym == inner->as.sym) {
                        return macro_arg_datum(e, args[i], mark);
                    }
                }
                if (macro->rest_param && macro->rest_param == inner->as.sym) {
                    return macro_arg_datum(e, args[macro->n_params], mark);
                }
            }
            return form_unquote(e->arena, f->span,
                                substitute_params(e, inner, macro, args, mark));
        }
        case F_UNQUOTE_SPLICING:
            return form_unquote_splicing(e->arena, f->span,
                                         substitute_params(e, f->as.list.items[0], macro, args, mark));
        case F_LIST: {
            /* Check for gensym call: (gensym) or (gensym prefix) */
            if (f->as.list.len >= 1 && f->as.list.items[0]->tag == F_SYM) {
                const Symbol *name = f->as.list.items[0]->as.sym;
                if (name == e->sym_gensym) {
                    /* This is a gensym call - generate a fresh symbol */
                    const char *prefix = gensym_prefix_from_call(f);
                    return form_sym(e->arena, f->span,
                                    gensym_fresh(e, prefix ? prefix : "g"));
                }
            }
            /* Not a gensym call - continue with normal processing */
            /* Check if any item is ~@ (unquote-splicing) — use splice path if so */
            bool has_splice = false;
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                if (f->as.list.items[i]->tag == F_UNQUOTE_SPLICING) {
                    has_splice = true; break;
                }
            }
            if (has_splice) {
                /* Two-pass splice: substitute each item, splicing F_UNQUOTE_SPLICING */
                uint32_t n_in = f->as.list.len;
                Form **temp = (Form **)arena_alloc(e->arena, n_in * sizeof(Form *));
                bool *is_splice = (bool *)arena_alloc(e->arena, n_in * sizeof(bool));
                bool *keep_splice_wrapper = (bool *)arena_alloc(e->arena, n_in * sizeof(bool));
                uint32_t n_out = 0;
                for (uint32_t i = 0; i < n_in; i++) {
                    Form *item = f->as.list.items[i];
                    if (item->tag == F_UNQUOTE_SPLICING) {
                        /* ~@ inner: substitute the inner form, then splice if it's a list */
                        Form *inner = item->as.list.items[0];
                        Form *subst = substitute_params(e, inner, macro, args, mark);
                        temp[i] = subst;
                        if (is_macro_arg_datum(subst) &&
                            subst->as.list.items[0]->tag == F_LIST) {
                            /* `~@rest`: the caller's argument list.  Splice
                             * its elements, each still marked as data, so
                             * the CT evaluator cannot evaluate them (or let
                             * them decide whether it runs at all). */
                            Form *lst = subst->as.list.items[0];
                            Form **marked = (lst->as.list.len == 0) ? NULL
                                : (Form **)arena_alloc(e->arena, lst->as.list.len * sizeof(Form *));
                            for (uint32_t j = 0; j < lst->as.list.len; j++)
                                marked[j] = macro_arg_datum(e, lst->as.list.items[j], true);
                            temp[i] = form_list(e->arena, lst->span, marked, lst->as.list.len);
                            is_splice[i] = true;
                            keep_splice_wrapper[i] = false;
                            n_out += lst->as.list.len;
                        } else if (splice_inner_needs_ct_eval(e, subst)) {
                            /* CT computation (e.g. (map ...) / a macro call):
                             * keep the wrapper so ct_eval_quasiquote computes
                             * and splices the result rather than flattening the
                             * un-evaluated call here. */
                            is_splice[i] = false;
                            keep_splice_wrapper[i] = true;
                            n_out++;
                        } else if (subst->tag == F_LIST) {
                            is_splice[i] = true;
                            keep_splice_wrapper[i] = false;
                            n_out += subst->as.list.len;
                        } else {
                            is_splice[i] = false;
                            keep_splice_wrapper[i] = true;
                            n_out++;
                        }
                    } else {
                        temp[i] = substitute_params(e, item, macro, args, mark);
                        is_splice[i] = false;
                        keep_splice_wrapper[i] = false;
                        n_out++;
                    }
                }
                Form **items = (Form **)arena_alloc(e->arena, n_out * sizeof(Form *));
                uint32_t out_idx = 0;
                for (uint32_t i = 0; i < n_in; i++) {
                    if (is_splice[i]) {
                        Form *lst = temp[i];
                        for (uint32_t j = 0; j < lst->as.list.len; j++) {
                            items[out_idx++] = lst->as.list.items[j];
                        }
                    } else {
                        if (keep_splice_wrapper[i]) {
                            items[out_idx++] = form_unquote_splicing(e->arena, f->as.list.items[i]->span, temp[i]);
                        } else {
                            items[out_idx++] = temp[i];
                        }
                    }
                }
                Form *spliced = form_list(e->arena, f->span, items, n_out);
                spliced->fx_prov = f->fx_prov;   /* keep #reads/#fx provenance */
                return spliced;
            }
            Form **items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                items[i] = substitute_params(e, f->as.list.items[i], macro, args, mark);
            }
            /* Preserve annotation provenance through the template rebuild: a
             * `#reads w` reads as `(reads w)` stamped PROV_READS, and
             * elab_defn's signature walk keys on the stamp -- without it the
             * annotation elaborates as a body call. */
            Form *rebuilt = form_list(e->arena, f->span, items, f->as.list.len);
            rebuilt->fx_prov = f->fx_prov;
            return rebuilt;
        }
        case F_SYM: {
            /* Check if this symbol is a fixed parameter - if so, replace with corresponding arg */
            for (uint32_t i = 0; i < macro->n_params; i++) {
                Form *param = macro->params[i];
                if (param->tag == F_SYM && param->as.sym == f->as.sym) {
                    /* ^syntax param: leave the symbol; the CT env (set up by
                     * elab_eval_macro_form) binds it to the raw arg Form so
                     * (first decl) / (symbol-name decl) treat it as AST data
                     * rather than evaluating it as code. */
                    if (macro->is_syntax_param && macro->is_syntax_param[i]) return f;
                    return macro_arg_datum(e, args[i], mark);
                }
            }
            /* Check if this symbol is the rest parameter */
            if (macro->rest_param && macro->rest_param == f->as.sym) {
                if (macro->rest_is_syntax) return f;
                /* rest list packed at n_params index */
                return macro_arg_datum(e, args[macro->n_params], mark);
            }
            /* Not a parameter - return as-is */
            return f;
        }
        case F_VEC:
        case F_MAP:
        case F_SET:
        case F_MAP_LITERAL:
        case F_SET_LITERAL:
        case F_ROW_LITERAL: {
            /* Recursively substitute in vector/map/set items */
            Form **items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) {
                items[i] = substitute_params(e, f->as.list.items[i], macro, args, mark);
            }
            if (f->tag == F_MAP) {
                Form *m = form_map(e->arena, f->span, items, f->as.list.len);
                m->fx_prov = f->fx_prov;   /* keep #fx{...} explicit-row provenance */
                return m;
            }
            if (f->tag == F_SET) return form_set(e->arena, f->span, items, f->as.list.len);
            if (f->tag == F_MAP_LITERAL) return form_map_literal(e->arena, f->span, items, f->as.list.len);
            if (f->tag == F_SET_LITERAL) return form_set_literal(e->arena, f->span, items, f->as.list.len);
            if (f->tag == F_ROW_LITERAL) return form_row_literal(e->arena, f->span, items, f->as.list.len);
            return form_vec(e->arena, f->span, items, f->as.list.len);
        }
    }
    return f;
}

static Form *elab_expand_macro_impl(Elab *e, MacroDef *macro, Form **args, uint32_t n_args);

/* --timings (profiling-plan C1): computing an expansion -- substitution,
 * compile-time evaluation, a procedural macro's closure -- is "macro-expand".
 * Elaborating the form it produced happens in the caller, after this returns,
 * and is "elaborate".  Nested expansions re-enter the same zone for free. */
Form *elab_expand_macro(Elab *e, MacroDef *macro, Form **args, uint32_t n_args) {
    prof_enter(PROF_MACRO_EXPAND);
    Form *r = elab_expand_macro_impl(e, macro, args, n_args);
    prof_exit(PROF_MACRO_EXPAND);
    return r;
}

/* Phase 6: Expand a macro call with arguments */
static Form *elab_expand_macro_impl(Elab *e, MacroDef *macro, Form **args, uint32_t n_args) {
    /* Phase M4: set expansion-origin module so private helper macros of the
     * same module are accessible during this expansion. Restore on every exit.
     * Cross-module wrapper-macro bug fix: push on a stack so nested expansions
     * preserve the outer module's visibility too. */
    const Symbol *saved_expansion_mod = e->macro_expansion_module;
    e->macro_expansion_module = macro->defining_module_name;
    if (e->n_macro_expansion_stack >= e->cap_macro_expansion_stack) {
        uint32_t nc = e->cap_macro_expansion_stack ? e->cap_macro_expansion_stack * 2 : 8;
        e->macro_expansion_stack = (const Symbol **)realloc(
            e->macro_expansion_stack, nc * sizeof(const Symbol *));
        e->cap_macro_expansion_stack = nc;
    }
    e->macro_expansion_stack[e->n_macro_expansion_stack++] =
        macro->defining_module_name;
    /* AI3.1 (application-image-dumps-plan): the image-cache combinator's
     * `init` root is the code whose global writes a warm start silently
     * drops.  Note it here, at the one place the call is still visible by
     * name; the check itself runs after the whole unit is elaborated. */
    if (n_args == 3 && args[1] && args[1]->tag == F_SYM && args[1]->as.sym &&
        macro->name && strcmp(macro->name->name, "with-image-cache-after-init") == 0)
        wf_note_image_cache_root(e, args[1]->as.sym, args[1]->span);

#define EXPAND_RESTORE() do { \
        e->macro_expansion_module = saved_expansion_mod; \
        if (e->n_macro_expansion_stack > 0) e->n_macro_expansion_stack--; \
    } while (0)

    /* Stage 2: a procedural macro's expansion is computed by its closure in
     * the macro-time env, not by substitution + CT eval.  Arity checking,
     * variadic packing, invocation, and the cross-symtab import of the
     * result all live in elab_macro_env_call_proc (src/turi/macro_env.c). */
    if (macro->kind == MACRO_PROCEDURAL) {
        Span call_span = n_args > 0 ? args[0]->span : macro->span;
        Form *result = elab_macro_env_call_proc(e, macro, args, n_args, call_span);
        EXPAND_RESTORE(); return result;
    }

    /* Check arity */
    if (macro->is_variadic) {
        if (n_args < macro->n_params) {
            diag_emit(DIAG_ERROR, macro->span,
                      "macro '%s' expects at least %u arguments, got %u",
                      macro->name->name, macro->n_params, n_args);
            EXPAND_RESTORE(); return NULL;
        }
        /* Build augmented args: fixed args + packed rest list at index n_params */
        Form **aug_args = (Form **)arena_alloc(e->arena, (macro->n_params + 1) * sizeof(Form *));
        for (uint32_t i = 0; i < macro->n_params; i++) {
            aug_args[i] = args[i];
        }
        /* Pack trailing args into an F_LIST for the rest param */
        uint32_t n_rest = n_args - macro->n_params;
        Form **rest_items = (Form **)arena_alloc(e->arena, n_rest * sizeof(Form *));
        for (uint32_t i = 0; i < n_rest; i++) {
            rest_items[i] = args[macro->n_params + i];
        }
        Span rest_span = n_rest > 0 ? args[macro->n_params]->span : macro->span;
        aug_args[macro->n_params] = form_list(e->arena, rest_span, rest_items, n_rest);
        Form *templ = substitute_params(e, macro->body, macro, aug_args, true);
        if (!templ) { EXPAND_RESTORE(); return NULL; }
        Form *result = (template_needs_ct_eval(templ) || macro->is_syntax_param ||
                        macro->rest_is_syntax || macro->force_ct_eval)
            ? elab_eval_macro_form(e, templ, macro, aug_args) : templ;
        result = strip_macro_arg_data(e, result);
        EXPAND_RESTORE(); return result;
    }
    if (n_args != macro->n_params) {
        diag_emit(DIAG_ERROR, macro->span,
                  "macro '%s' expects %u arguments, got %u",
                  macro->name->name, macro->n_params, n_args);
        EXPAND_RESTORE(); return NULL;
    }

    /* Substitute parameters in the macro body */
    Form *templ = substitute_params(e, macro->body, macro, args, true);
    if (!templ) { EXPAND_RESTORE(); return NULL; }
    Form *result = (template_needs_ct_eval(templ) || macro->is_syntax_param ||
                    macro->force_ct_eval)
        ? elab_eval_macro_form(e, templ, macro, args) : templ;
    result = strip_macro_arg_data(e, result);
    EXPAND_RESTORE(); return result;

#undef EXPAND_RESTORE
}

/* Phase 6: defmacro — (defmacro name [params...] body...)
 * Defines a macro that will be expanded at compile time.
 * Syntax: (defmacro name [param1 param2 ...] body...)
 * The macro body can use quasiquote/unquote to build the output form.
 * The macro is stored and will be used to expand subsequent calls.
 */
Expr *elab_defmacro(Elab *e, const Form *call) {
    /* Minimum: (defmacro name [params...] body...) */
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmacro requires (defmacro name [params...] body...)");
        return NULL;
    }

    /* Parse name */
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defmacro name must be a symbol");
        return NULL;
    }

    /* TUR-W0042: macro dispatch happens AFTER special-form dispatch in
     * elab_call, so a macro named after a reserved special form is never
     * expanded at a bare call site.  Same warning, same reasoning as the defn
     * path (docs/archive/history/defn-shadows-return-special-form.md). */
    if (!e->in_stdlib_load)
        tur_warn_if_shadows_special_form(name_f->as.sym, name_f->span, "defmacro");

    /* Check if macro already exists.  PS4: an earlier REPL/playground turn's
     * macro is replaced rather than refused. */
    {
        MacroDef *prior = elab_lookup_macro(e, name_f->as.sym);
        if (prior && elab_prior_turn_macro(e, prior)) {
            elab_remove_macro(e, prior);
        } else if (prior) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defmacro: '%s' is already defined", name_f->as.sym->name);
            return NULL;
        }
    }

    /* Parse params */
    Form *params_f = call->as.list.items[2];
    if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defmacro: expected parameter list or vector, got %s", 
                  params_f->tag == F_SYM ? "symbol" : "non-list");
        return NULL;
    }

    /* Extract parameter symbols, detecting optional variadic '& rest' tail
     * and per-param '^syntax' markers (macro-args-elaborated-before-expansion).
     * '^syntax' appears as a separate token immediately before the param it
     * marks; the next token is the actual param name (may also follow '&'). */
    uint32_t n_total = params_f->as.list.len;
    /* Allocate enough space for all params (excluding '&' / '^syntax' markers) */
    Form **params = (Form **)arena_alloc(e->arena, n_total * sizeof(Form *));
    bool *is_syntax_param = (bool *)arena_alloc(e->arena, n_total * sizeof(bool));
    memset(is_syntax_param, 0, n_total * sizeof(bool));
    uint32_t n_fixed = 0;
    bool is_variadic = false;
    bool rest_is_syntax = false;
    const Symbol *rest_param = NULL;
    bool next_is_syntax = false;
    bool any_syntax = false;

    for (uint32_t i = 0; i < n_total; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "defmacro: parameter must be a symbol, got %s",
                      p->tag == F_KEYWORD ? "keyword" : "non-symbol");
            return NULL;
        }
        /* '^syntax' marker: the next param receives its arg as raw AST. */
        if (p->as.sym->len == 7 && memcmp(p->as.sym->name, "^syntax", 7) == 0) {
            if (i + 1 >= n_total) {
                diag_emit(DIAG_ERROR, p->span,
                          "defmacro: '^syntax' must be followed by a parameter name");
                return NULL;
            }
            next_is_syntax = true;
            continue;
        }
        if (p->as.sym == e->sym_borrow) {
            /* '&' rest-arg marker: the next symbol (or '^syntax' marker + symbol)
             * is the rest param */
            uint32_t j = i + 1;
            bool rest_syn = next_is_syntax;
            next_is_syntax = false;
            if (j < n_total) {
                Form *q = params_f->as.list.items[j];
                if (q->tag == F_SYM && q->as.sym->len == 7 &&
                    memcmp(q->as.sym->name, "^syntax", 7) == 0) {
                    rest_syn = true;
                    j++;
                }
            }
            if (j >= n_total) {
                diag_emit(DIAG_ERROR, p->span,
                          "defmacro: '&' must be followed by a rest parameter name");
                return NULL;
            }
            if (j + 1 < n_total) {
                diag_emit(DIAG_ERROR, params_f->as.list.items[j + 1]->span,
                          "defmacro: only one parameter may follow '&'");
                return NULL;
            }
            Form *rest_f = params_f->as.list.items[j];
            if (rest_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, rest_f->span,
                          "defmacro: rest parameter after '&' must be a symbol");
                return NULL;
            }
            is_variadic = true;
            rest_param = rest_f->as.sym;
            rest_is_syntax = rest_syn;
            if (rest_syn) any_syntax = true;
            break; /* no more params after & rest */
        }
        if (next_is_syntax) {
            is_syntax_param[n_fixed] = true;
            any_syntax = true;
            next_is_syntax = false;
        }
        params[n_fixed++] = p;
    }
    if (next_is_syntax) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defmacro: '^syntax' must be followed by a parameter name");
        return NULL;
    }

    /* The body is everything after the parameter list */
    uint32_t body_count = call->as.list.len - 3;
    if (body_count == 0) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmacro: expected at least one body expression");
        return NULL;
    }

    /* Multi-form bodies: earlier forms are compile-time setup (evaluated in
     * order by the CT evaluator's `do`, with (def x v) forms spliced into
     * let bindings), and the LAST form's value is the template.  Wrap them
     * in a synthetic (do ...) and remember to force the template through
     * the CT evaluator -- a (do setup... template) has no meaning as a
     * literal runtime template. */
    Form *body;
    bool multi_form_body = body_count > 1;
    if (multi_form_body) {
        Form **body_items = (Form **)arena_alloc(e->arena,
                                                 (body_count + 1) * sizeof(Form *));
        body_items[0] = form_sym(e->arena, call->span, e->sym_do);
        for (uint32_t i = 0; i < body_count; i++) {
            body_items[i + 1] = call->as.list.items[3 + i];
        }
        body = form_list(e->arena, call->span, body_items, body_count + 1);
    } else {
        body = call->as.list.items[3];
    }

    /* Create the macro definition.  arena_alloc returns uninitialised
     * memory, so memset first -- the `is_referred` bool is set lazily
     * by the :refer alias path in elab_module.c and would otherwise
     * read as garbage at lookup time (UBSan: "load of value X which
     * is not a valid value for type _Bool"). */
    MacroDef *macro = (MacroDef *)arena_alloc(e->arena, sizeof(MacroDef));
    memset(macro, 0, sizeof(MacroDef));
    macro->name = name_f->as.sym;
    macro->params = params;
    macro->n_params = n_fixed;
    macro->is_variadic = is_variadic;
    macro->rest_param = rest_param;
    macro->is_syntax_param = any_syntax ? is_syntax_param : NULL;
    macro->rest_is_syntax = rest_is_syntax;
    macro->body = body;
    macro->force_ct_eval = multi_form_body;
    macro->span = call->span;
    macro->defining_module_name = e->current_module_name; /* Phase M4 */

    /* Register the macro */
    elab_register_macro(e, macro);

    /* Return nil - defmacro doesn't produce a value */
    return e_nil(e, call->span);
}

/* ---------------------------------------------------------------------------
 * Stage 2 follow-up: quasiquote producing Syntax inside defmacro* bodies.
 *
 * A quasiquote in a defmacro* body is sugar for the syntax constructors:
 *
 *   `(+ ~e ~e)        lowers to  (syntax-list (sym->syntax "+") e e)
 *   `(f ~@xs 9)       lowers to  (syntax-append (syntax-list (sym->syntax "f"))
 *                                               xs
 *                                               (syntax-list (int->syntax 9)))
 *
 * `~expr` splices a Syntax-valued expression verbatim; `~@expr` splices the
 * elements of a list-shaped Syntax.  The lowering is purely syntactic and
 * happens BEFORE the body is handed to the macro-time env, so the env's
 * elaborator sees only ordinary calls -- no evaluator-mode special cases.
 * ------------------------------------------------------------------------- */

static Form *sxqq_callv(Elab *e, Span sp, const char *fn,
                        Form **args, uint32_t n_args) {
    Form **items = (Form **)arena_alloc(e->arena, (n_args + 1) * sizeof(Form *));
    items[0] = form_sym(e->arena, sp,
                        symtab_intern(e->st, strslice(fn, (uint32_t)strlen(fn))));
    for (uint32_t i = 0; i < n_args; i++) items[i + 1] = args[i];
    return form_list(e->arena, sp, items, n_args + 1);
}

static Form *sxqq_call1(Elab *e, Span sp, const char *fn, Form *arg) {
    return sxqq_callv(e, sp, fn, &arg, 1);
}

static Form *sxqq_call_strlit(Elab *e, Span sp, const char *fn,
                              const char *s, uint32_t len) {
    Form *lit = form_str(e->arena, sp, s, len);
    return sxqq_call1(e, sp, fn, lit);
}

static Form *sxqq_lower(Elab *e, Form *f, bool *ok);
static Form *sxqq_lower_quoted(Elab *e, Form *f, bool *ok);

/* Lower the items of a list/vec template body.  With no `~@` present the
 * result is one (syntax-list ...) / (syntax-vec ...) call; a splice turns
 * the list case into (syntax-append seg...) where runs of plain items group
 * into (syntax-list ...) segments and each `~@e` contributes `e` verbatim. */
static Form *sxqq_lower_seq(Elab *e, Form *f, bool is_vec, bool quoted, bool *ok) {
    uint32_t n = f->as.list.len;
    bool has_splice = false;
    if (!quoted)
        for (uint32_t i = 0; i < n; i++)
            if (f->as.list.items[i]->tag == F_UNQUOTE_SPLICING) { has_splice = true; break; }

    if (!has_splice) {
        Form **items = (n == 0) ? NULL
            : (Form **)arena_alloc(e->arena, n * sizeof(Form *));
        for (uint32_t i = 0; i < n; i++) {
            items[i] = quoted ? sxqq_lower_quoted(e, f->as.list.items[i], ok)
                              : sxqq_lower(e, f->as.list.items[i], ok);
            if (!*ok) return f;
        }
        return sxqq_callv(e, f->span, is_vec ? "syntax-vec" : "syntax-list",
                          items, n);
    }
    if (is_vec) {
        diag_emit(DIAG_ERROR, f->span,
                  "defmacro* quasiquote: '~@' splicing into a vector template "
                  "is not supported; build it with syntax-vec explicitly");
        *ok = false;
        return f;
    }
    /* Segment the items around the splices. */
    Form **parts = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
    Form **seg   = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
    uint32_t n_parts = 0, seg_len = 0;
    for (uint32_t i = 0; i < n; i++) {
        Form *item = f->as.list.items[i];
        if (item->tag == F_UNQUOTE_SPLICING) {
            if (seg_len > 0) {
                parts[n_parts++] = sxqq_callv(e, f->span, "syntax-list", seg, seg_len);
                seg = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
                seg_len = 0;
            }
            parts[n_parts++] = item->as.list.items[0];  /* the spliced expr, verbatim */
        } else {
            seg[seg_len++] = sxqq_lower(e, item, ok);
            if (!*ok) return f;
        }
    }
    if (seg_len > 0)
        parts[n_parts++] = sxqq_callv(e, f->span, "syntax-list", seg, seg_len);
    return sxqq_callv(e, f->span, "syntax-append", parts, n_parts);
}

static Form *sxqq_lower(Elab *e, Form *f, bool *ok) {
    switch (f->tag) {
        case F_SYM:
            return sxqq_call_strlit(e, f->span, "sym->syntax",
                                    f->as.sym->name, f->as.sym->len);
        case F_KEYWORD:
            return sxqq_call_strlit(e, f->span, "kw->syntax",
                                    f->as.sym->name, f->as.sym->len);
        case F_STR:
            return sxqq_call_strlit(e, f->span, "str->syntax",
                                    f->as.s.p, f->as.s.len);
        case F_INT:
            return sxqq_call1(e, f->span, "int->syntax",
                              form_int(e->arena, f->span, f->as.i));
        case F_FLOAT:
            return sxqq_call1(e, f->span, "float->syntax",
                              form_float(e->arena, f->span, f->as.f));
        case F_BOOL:
            return sxqq_call1(e, f->span, "bool->syntax",
                              form_bool(e->arena, f->span, f->as.b));
        case F_NIL:
            return sxqq_callv(e, f->span, "nil->syntax", NULL, 0);
        case F_UNQUOTE:
            /* `~expr` -- the expression itself, evaluated by the macro body
             * (must be Syntax-typed; checked at definition time). */
            return f->as.list.items[0];
        case F_UNQUOTE_SPLICING:
            diag_emit(DIAG_ERROR, f->span,
                      "defmacro* quasiquote: '~@' is only meaningful inside a "
                      "list template");
            *ok = false;
            return f;
        case F_LIST:
            return sxqq_lower_seq(e, f, false, false, ok);
        case F_VEC:
            return sxqq_lower_seq(e, f, true, false, ok);
        case F_TYPE_ANN:
            return sxqq_call1(e, f->span, "syntax-type-ann",
                              sxqq_lower(e, f->as.list.items[0], ok));
        case F_QUOTE:
            return sxqq_call1(e, f->span, "syntax-quote",
                              sxqq_lower(e, f->as.list.items[0], ok));
        case F_QUASIQUOTE:
            diag_emit(DIAG_ERROR, f->span,
                      "defmacro* quasiquote: nested quasiquote is not "
                      "supported; build the inner template with the syntax "
                      "constructors");
            *ok = false;
            return f;
        default:
            diag_emit(DIAG_ERROR, f->span,
                      "defmacro* quasiquote: a %s template is not supported "
                      "here; build it with the syntax constructors (or use a "
                      "template defmacro)",
                      form_tag_name(f->tag));
            *ok = false;
            return f;
    }
}

/* quote-list-data-plan (graduation item 3): a quoted list in a defmacro*
 * body is the Syntax it spells, not a runtime Datum.  The body computes
 * forms, so `'(+ 3 4)` means what `` `(+ 3 4) `` does -- the quasiquote with
 * nothing unquoted -- and can be returned as the expansion or compared with
 * `syntax=?`.  The leaves lower as sxqq_lower's do; a nested quote is
 * `syntax-quote`; an unquote inside a quote has no constructor, so it is
 * refused rather than silently spliced. */
static Form *sxqq_lower_quoted(Elab *e, Form *f, bool *ok) {
    switch (f->tag) {
        case F_LIST:
            return sxqq_lower_seq(e, f, false, true, ok);
        case F_VEC:
            return sxqq_lower_seq(e, f, true, true, ok);
        case F_TYPE_ANN:
            return sxqq_call1(e, f->span, "syntax-type-ann",
                              sxqq_lower_quoted(e, f->as.list.items[0], ok));
        case F_QUOTE:
            return sxqq_call1(e, f->span, "syntax-quote",
                              sxqq_lower_quoted(e, f->as.list.items[0], ok));
        case F_QUASIQUOTE:
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING:
            diag_emit(DIAG_ERROR, f->span,
                      "defmacro*: a quoted list holding a %s has no Syntax "
                      "constructor; use a quasiquote template to splice, or "
                      "build it with the syntax constructors",
                      form_tag_name(f->tag));
            *ok = false;
            return f;
        default:
            return sxqq_lower(e, f, ok);
    }
}

/* Walk a defmacro* body form, lowering every quasiquote -- and, under
 * --enable=quote-data, every quoted list or vector (sxqq_lower_quoted).  A
 * quoted symbol stays the `Sym` it is everywhere else; everything
 * list-shaped recurses. */
static Form *sxqq_walk(Elab *e, Form *f, bool *ok) {
    switch (f->tag) {
        case F_QUASIQUOTE:
            return sxqq_lower(e, f->as.list.items[0], ok);
        case F_QUOTE:
            if (g_opt_quote_data && f->as.list.len == 1 &&
                (f->as.list.items[0]->tag == F_LIST || f->as.list.items[0]->tag == F_VEC)) {
                experiment_warn_if_used("quote-data");
                return sxqq_lower_quoted(e, f->as.list.items[0], ok);
            }
            return f;
        case F_LIST:
            /* `(quote (a b))` written out means `'(a b)`. */
            if (g_opt_quote_data && f->as.list.len == 2 &&
                f->as.list.items[0]->tag == F_SYM &&
                strcmp(f->as.list.items[0]->as.sym->name, "quote") == 0 &&
                (f->as.list.items[1]->tag == F_LIST || f->as.list.items[1]->tag == F_VEC)) {
                experiment_warn_if_used("quote-data");
                return sxqq_lower_quoted(e, f->as.list.items[1], ok);
            }
            /* fallthrough */
        case F_VEC:
        case F_MAP:
        case F_SET:
        case F_MAP_LITERAL:
        case F_SET_LITERAL:
        case F_ROW_LITERAL:
        case F_TYPE_ANN:
        case F_CONTRACT_TYPE:
        case F_READER_COND:
        case F_RANGE_VAR:
        case F_UNQUOTE:
        case F_UNQUOTE_SPLICING: {
            uint32_t n = f->as.list.len;
            Form **items = (n == 0) ? NULL
                : (Form **)arena_alloc(e->arena, n * sizeof(Form *));
            bool changed = false;
            for (uint32_t i = 0; i < n; i++) {
                items[i] = sxqq_walk(e, f->as.list.items[i], ok);
                if (!*ok) return f;
                if (items[i] != f->as.list.items[i]) changed = true;
            }
            if (!changed) return f;
            Form *out = form_new(e->arena, f->tag, f->span);
            out->lit_suffix = f->lit_suffix;
            out->fx_prov    = f->fx_prov;
            out->as.list.items = items;
            out->as.list.len   = n;
            return out;
        }
        default:
            return f;
    }
}

/* Stage 2 (macro-system-direction-plan): defmacro* -- procedural macros.
 *
 *   (defmacro* name [params...] body...)
 *
 * The body is ordinary Turmeric, not a template: it receives each argument
 * as a Syntax value (the raw call-site form), computes with the full
 * language plus the syntax natives (syntax-first, syntax->int,
 * int->syntax, syntax-list, ...), and returns the expansion as a Syntax.
 * At definition time the body is compiled into the session's macro-time
 * turi env as
 *
 *   (defn <name>__mx [p1 : Syntax ... ] : Syntax body...)
 *
 * so parameter/return types are checked when the macro is DEFINED, and the
 * closure is invoked at each expansion (elab_macro_env_call_proc).  A
 * variadic `& rest` param is declared as one ordinary Syntax param -- the
 * call path packs the trailing argument forms into a single list form.
 */
Expr *elab_defmacro_star(Elab *e, const Form *call) {
    if (call->as.list.len < 4) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmacro* requires (defmacro* name [params...] body...)");
        return NULL;
    }

    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defmacro* name must be a symbol");
        return NULL;
    }
    if (!e->in_stdlib_load)
        tur_warn_if_shadows_special_form(name_f->as.sym, name_f->span, "defmacro*");
    {
        MacroDef *prior = elab_lookup_macro(e, name_f->as.sym);
        if (prior && elab_prior_turn_macro(e, prior)) {   /* PS4: see defmacro */
            elab_remove_macro(e, prior);
        } else if (prior) {
            diag_emit(DIAG_ERROR, name_f->span,
                      "defmacro*: '%s' is already defined", name_f->as.sym->name);
            return NULL;
        }
    }

    Form *params_f = call->as.list.items[2];
    if (params_f->tag != F_LIST && params_f->tag != F_VEC) {
        diag_emit(DIAG_ERROR, params_f->span,
                  "defmacro*: expected parameter list or vector");
        return NULL;
    }

    /* Params are bare symbols, optionally ending `& rest`.  No `^syntax`
     * markers: every defmacro* parameter is already raw syntax. */
    uint32_t n_total = params_f->as.list.len;
    Form **params = (Form **)arena_alloc(e->arena, n_total * sizeof(Form *));
    uint32_t n_fixed = 0;
    bool is_variadic = false;
    const Symbol *rest_param = NULL;
    for (uint32_t i = 0; i < n_total; i++) {
        Form *p = params_f->as.list.items[i];
        if (p->tag != F_SYM) {
            diag_emit(DIAG_ERROR, p->span,
                      "defmacro*: parameter must be a bare symbol "
                      "(every parameter is already Syntax; type annotations "
                      "and ^syntax markers are not accepted)");
            return NULL;
        }
        if (p->as.sym->len == 7 && memcmp(p->as.sym->name, "^syntax", 7) == 0) {
            diag_emit(DIAG_ERROR, p->span,
                      "defmacro*: '^syntax' is meaningless here -- every "
                      "defmacro* parameter already receives raw syntax");
            return NULL;
        }
        if (p->as.sym == e->sym_borrow) {
            if (i + 1 >= n_total) {
                diag_emit(DIAG_ERROR, p->span,
                          "defmacro*: '&' must be followed by a rest parameter name");
                return NULL;
            }
            if (i + 2 < n_total) {
                diag_emit(DIAG_ERROR, params_f->as.list.items[i + 2]->span,
                          "defmacro*: only one parameter may follow '&'");
                return NULL;
            }
            Form *rest_f = params_f->as.list.items[i + 1];
            if (rest_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, rest_f->span,
                          "defmacro*: rest parameter after '&' must be a symbol");
                return NULL;
            }
            is_variadic = true;
            rest_param = rest_f->as.sym;
            break;
        }
        params[n_fixed++] = p;
    }

    uint32_t body_count = call->as.list.len - 3;

    /* Bound closure name: <name>__mx. */
    size_t fn_len = name_f->as.sym->len + 4;
    char *fn_name = (char *)arena_alloc(e->arena, fn_len + 1);
    memcpy(fn_name, name_f->as.sym->name, name_f->as.sym->len);
    memcpy(fn_name + name_f->as.sym->len, "__mx", 5);

    /* Synthesize (defn <fn_name> [p : Syntax ...] : Syntax body...).  The
     * spaced `: T` annotation is an F_TYPE_ANN item following the slot it
     * annotates, both in the param vector and after it (return type). */
    const Symbol *syntax_sym = symtab_intern(e->st, strslice("Syntax", 6));
    const Symbol *fn_sym = symtab_intern(e->st, strslice(fn_name, (uint32_t)fn_len));
    Span sp = call->span;
    Form *syntax_ann_proto = form_sym(e->arena, sp, syntax_sym);

    uint32_t n_decl = n_fixed + (is_variadic ? 1 : 0);
    Form **pv = (Form **)arena_alloc(e->arena, (n_decl * 2) * sizeof(Form *));
    uint32_t pvn = 0;
    for (uint32_t i = 0; i < n_fixed; i++) {
        pv[pvn++] = params[i];
        pv[pvn++] = form_type_ann(e->arena, params[i]->span, syntax_ann_proto);
    }
    if (is_variadic) {
        pv[pvn++] = form_sym(e->arena, sp, rest_param);
        pv[pvn++] = form_type_ann(e->arena, sp, syntax_ann_proto);
    }

    uint32_t defn_len = 4 + body_count;
    Form **items = (Form **)arena_alloc(e->arena, defn_len * sizeof(Form *));
    items[0] = form_sym(e->arena, sp, e->sym_defn);
    items[1] = form_sym(e->arena, sp, fn_sym);
    items[2] = form_vec(e->arena, params_f->span, pv, pvn);
    items[3] = form_type_ann(e->arena, sp, syntax_ann_proto);
    /* Quasiquote sugar: lower every `template` in the body into syntax
     * constructor calls before the body reaches the macro-time env. */
    bool qq_ok = true;
    for (uint32_t i = 0; i < body_count; i++) {
        items[4 + i] = sxqq_walk(e, call->as.list.items[3 + i], &qq_ok);
        if (!qq_ok) return NULL;
    }
    Form *defn_form = form_list(e->arena, sp, items, defn_len);

    if (!elab_macro_env_define_proc(e, fn_name, defn_form, name_f->span))
        return NULL;

    MacroDef *macro = (MacroDef *)arena_alloc(e->arena, sizeof(MacroDef));
    memset(macro, 0, sizeof(MacroDef));
    macro->name = name_f->as.sym;
    macro->params = params;
    macro->n_params = n_fixed;
    macro->is_variadic = is_variadic;
    macro->rest_param = rest_param;
    macro->body = call->as.list.items[3];  /* first body form, for tooling */
    macro->span = call->span;
    macro->defining_module_name = e->current_module_name;
    macro->kind = MACRO_PROCEDURAL;
    macro->proc_fn_name = fn_name;
    elab_register_macro(e, macro);

    return e_nil(e, call->span);
}

/* Phase 6: gensym — (gensym) or (gensym prefix-sym) generates a fresh symbol Form */
Expr *elab_gensym(Elab *e, const Form *call) {
    /* Syntax: (gensym) or (gensym prefix) */
    const char *prefix = gensym_prefix_from_call(call);
    if (!prefix) {
        diag_emit(DIAG_ERROR, call->as.list.items[1]->span,
                  "gensym: expected symbol, string, or quoted symbol prefix, got %s",
                  call->as.list.items[1]->tag == F_INT ? "integer" : "other");
        return NULL;
    }

    /* A (gensym ...) call that reaches ordinary elaboration sits in runtime
     * code: template substitution and quasiquote expansion intercept every
     * gensym inside macro machinery before elab_form ever sees it.  A fresh
     * symbol has no runtime value, so reject it plainly instead of the old
     * behavior of minting an unbound TYPE_INT variable that failed later in
     * codegen with an undeclared-identifier C error. */
    diag_emit(DIAG_ERROR, call->span,
              "(gensym) has no runtime value: it mints a fresh symbol for use "
              "inside a macro template or quasiquote, where it is expanded at "
              "compile time");
    return NULL;
}
