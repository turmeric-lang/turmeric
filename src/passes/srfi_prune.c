/* srfi_prune.c -- drop the SRFI definitions a program never reaches
 * (r7rs-srfi-plan S3).  See srfi_prune.h for what and why. */
#include "srfi_prune.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "typeclass.h"

/* ---------------------------------------------------------------------------
 * A set of pointers (open addressing), and a map from a Binding to the index
 * of the candidate item that defines it.
 * ------------------------------------------------------------------------- */
typedef struct PSet {
    const void **keys;
    uint32_t    *vals;
    uint32_t     cap, n;
} PSet;

static uint32_t pset_hash(const void *p, uint32_t cap) {
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 17; x *= 0xed5ad4bbu; x ^= x >> 11; x *= 0xac4c1b51u; x ^= x >> 15;
    return (uint32_t)x & (cap - 1);
}

static void pset_grow(PSet *s);

/* Insert p (with val); true when it was not there before. */
static bool pset_put(PSet *s, const void *p, uint32_t val) {
    if ((s->n + 1) * 2 > s->cap) pset_grow(s);
    uint32_t i = pset_hash(p, s->cap);
    while (s->keys[i]) {
        if (s->keys[i] == p) return false;
        i = (i + 1) & (s->cap - 1);
    }
    s->keys[i] = p;
    s->vals[i] = val;
    s->n++;
    return true;
}

static bool pset_get(const PSet *s, const void *p, uint32_t *val) {
    if (!s->cap) return false;
    uint32_t i = pset_hash(p, s->cap);
    while (s->keys[i]) {
        if (s->keys[i] == p) { if (val) *val = s->vals[i]; return true; }
        i = (i + 1) & (s->cap - 1);
    }
    return false;
}

/* Insert or overwrite p's value. */
static void pset_set(PSet *s, const void *p, uint32_t val) {
    if (pset_put(s, p, val)) return;
    uint32_t i = pset_hash(p, s->cap);
    while (s->keys[i] != p) i = (i + 1) & (s->cap - 1);
    s->vals[i] = val;
}

static void pset_grow(PSet *s) {
    PSet old = *s;
    s->cap = old.cap ? old.cap * 2 : 256;
    s->n = 0;
    s->keys = (const void **)calloc(s->cap, sizeof *s->keys);
    s->vals = (uint32_t *)calloc(s->cap, sizeof *s->vals);
    for (uint32_t i = 0; i < old.cap; i++)
        if (old.keys[i]) pset_put(s, old.keys[i], old.vals[i]);
    free(old.keys);
    free(old.vals);
}

static void pset_free(PSet *s) {
    free(s->keys);
    free(s->vals);
    memset(s, 0, sizeof *s);
}

/* ---------------------------------------------------------------------------
 * The items: every top-level definition, with EX_DEFMODULE bodies flattened
 * in.  `slot` is where the item sits, so the rewrite can drop it in place.
 * ------------------------------------------------------------------------- */
typedef struct Item {
    Expr   **slot;       /* program items[] or a module's body[] entry */
    bool     candidate;  /* an SRFI definition that may be dropped */
    bool     live;       /* reached (always true for a non-candidate) */
    uint32_t from;       /* debug: the item whose walk reached it */
    int      from_kind;  /* debug: the node kind that named it */
    const Binding *from_via; /* debug: the binding whose link reached it */
} Item;

typedef struct Pr {
    Item        *items;
    uint32_t     n_items, cap_items;
    PSet         cand_of;   /* Binding* -> candidate item index */
    PSet         reached;   /* Binding* reached */
    PSet         walked;    /* FnDef* / GenDef* / TypeClassInstance* / Expr* walked */
    uint32_t    *work;      /* candidate items to walk */
    uint32_t     n_work, cap_work;
    const Expr **stack;     /* the walk's explicit stack */
    uint32_t     sp, cap_stack;
    bool         gave_up;   /* met an expression kind the walk does not model */
    uint32_t     cur;       /* the item being walked */
    int          cur_kind;  /* debug: the node being visited */
    const Binding *via;     /* debug: the binding whose links are followed */
} Pr;

static void add_item(Pr *p, Expr **slot) {
    if (p->n_items == p->cap_items) {
        p->cap_items = p->cap_items ? p->cap_items * 2 : 1024;
        p->items = (Item *)realloc(p->items, p->cap_items * sizeof *p->items);
    }
    p->items[p->n_items++] = (Item){ slot, false, true, 0, 0, NULL };
}

static void collect_items(Pr *p, Expr **arr, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (!arr[i]) continue;
        if (arr[i]->kind == EX_DEFMODULE && arr[i]->as.defmodule_.mod) {
            DefModule *m = arr[i]->as.defmodule_.mod;
            collect_items(p, m->body, m->n_body);
        } else {
            add_item(p, &arr[i]);
        }
    }
}

static bool srfi_file_span(Span sp) {
    const SourceFile *f = diag_source_file(sp.file_id);
    return f && f->path && strstr(f->path, "stdlib/srfi/") != NULL;
}

/* A `def` whose initializer is dropped with it must do nothing but make its
 * value: a literal, a reference, a lambda, or a representation wrapper around
 * one of those.  Anything that calls stays, whatever it calls. */
static bool init_has_no_effect(const Expr *e) {
    if (!e) return true;
    switch (e->kind) {
        case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT: case EX_FLOAT_LIT:
        case EX_CSTR_LIT: case EX_SYM_LIT: case EX_DEFAULT_OF:
        case EX_VAR: case EX_FN: case EX_CLOSURE:
            return true;
        case EX_ASCRIBE:      return init_has_no_effect(e->as.ascribe_.inner);
        case EX_UNION_INJECT: return init_has_no_effect(e->as.union_inject_.value);
        case EX_FN_TO_FAT:    return init_has_no_effect(e->as.fn_to_fat_.inner);
        case EX_POLY_TO_FAT:  return init_has_no_effect(e->as.poly_to_fat_.inner);
        case EX_POLY_WRAP:    return init_has_no_effect(e->as.poly_wrap_.inner);
        case EX_CAST:         return init_has_no_effect(e->as.cast_.expr);
        case EX_REINTERPRET:  return init_has_no_effect(e->as.reinterpret_.expr);
        default:              return false;
    }
}

static Binding *item_binding(const Expr *e) {
    if (e->kind == EX_FN_DEF && e->as.fn_def_.fn) return e->as.fn_def_.fn->binding;
    if (e->kind == EX_DEF) return e->as.def_.binding;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Allocation-only initializers (r7rs-srfi-18-216-sicp-plan T0b)
 *
 * A `def` whose initializer CALLS may still be dropped when nothing the call
 * does can be seen once its result is gone: it only allocates.  SRFI 27's
 * `(define default-random-source (make-random-source))` is the case that
 * asks for it -- a record of closures over a fresh state vector -- and it
 * was the root that kept the whole generator in every program importing
 * (srfi 27), or (srfi 216) over it.
 *
 * The check is deliberately narrow.  An expression is allocation-only when
 * it is a literal, a reference, a lambda (made, not run), a constructor or
 * a binding/sequencing form over allocation-only parts, a `set!` of a local,
 * or a direct call -- arguments allocation-only -- to:
 *
 *   - a top-level procedure defined in a stdlib/srfi/ file whose body is
 *     allocation-only (recursion, direct or mutual, is not: it might not
 *     return), or
 *   - one of the prelude's copying constructors in PURE_PRELUDE.
 *
 * Anything else -- an indirect call, a builtin, a global `set!`, I/O,
 * control -- is an effect, and the `def` stays.
 * ------------------------------------------------------------------------- */
static const char *const PURE_PRELUDE[] = {
    "r7rs-list->vector", "r7rs-vector->list", "r7rs-vector-copy", "r7rs-vector", "r7rs-list",
};

typedef struct Purity {
    PSet fn_of;   /* Binding* -> index into fns[] (stdlib/srfi/ top-level defns) */
    const FnDef **fns;
    uint32_t n_fns, cap_fns;
    PSet state;   /* FnDef* -> 1 in progress, 2 allocation-only, 3 not */
} Purity;

static bool prelude_pure(const Binding *b) {
    if (!b || !b->is_global || !b->name) return false;
    const SourceFile *f = diag_source_file(b->span.file_id);
    if (!f || !f->path) return false;
    size_t n = strlen(f->path), m = strlen("stdlib/r7rs/prelude.tur");
    if (n < m || strcmp(f->path + n - m, "stdlib/r7rs/prelude.tur") != 0) return false;
    for (size_t i = 0; i < sizeof PURE_PRELUDE / sizeof PURE_PRELUDE[0]; i++)
        if (strcmp(b->name->name, PURE_PRELUDE[i]) == 0) return true;
    return false;
}

static bool alloc_only(Purity *pu, const Expr *e);

static bool fn_alloc_only(Purity *pu, const Binding *b) {
    if (prelude_pure(b)) return true;
    uint32_t idx;
    if (!b || !pset_get(&pu->fn_of, b, &idx)) return false;
    const FnDef *fd = pu->fns[idx];
    uint32_t st;
    if (pset_get(&pu->state, fd, &st)) return st == 2;
    pset_put(&pu->state, fd, 1);
    bool ok = alloc_only(pu, fd->body);
    pset_set(&pu->state, fd, ok ? 2 : 3);
    return ok;
}

/* A call of a constructor of the ADT (a lowered defstruct included) the call
 * makes: a global with no function body, named like one of its variants. */
static bool ctor_call(const Expr *e) {
    const Binding *b = e->as.call_.fn_binding;
    if (!b || !b->is_global || b->source_fn_def || !b->name) return false;
    const Type *t = &e->type;
    while (t && t->kind == TY_APP) t = t->as.app.fn;   /* (Cons A): its head */
    if (!t || t->kind != TY_ADT || !t->as.adt_.def) return false;
    const AdtDef *d = t->as.adt_.def;
    for (uint32_t i = 0; i < d->n_ctors; i++)
        if (d->ctors[i] && d->ctors[i]->name && strcmp(d->ctors[i]->name, b->name->name) == 0) return true;
    return false;
}

static bool alloc_only_n(Purity *pu, Expr *const *arr, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (arr && !alloc_only(pu, arr[i])) return false;
    return true;
}

static bool alloc_only(Purity *pu, const Expr *e) {
    if (!e) return true;
    switch (e->kind) {
        case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT: case EX_FLOAT_LIT:
        case EX_CSTR_LIT: case EX_SYM_LIT: case EX_DEFAULT_OF:
        case EX_VAR: case EX_FN: case EX_CLOSURE:
            return true;
        case EX_ASCRIBE:      return alloc_only(pu, e->as.ascribe_.inner);
        case EX_UNION_INJECT: return alloc_only(pu, e->as.union_inject_.value);
        case EX_FN_TO_FAT:    return alloc_only(pu, e->as.fn_to_fat_.inner);
        case EX_POLY_TO_FAT:  return alloc_only(pu, e->as.poly_to_fat_.inner);
        case EX_POLY_WRAP:    return alloc_only(pu, e->as.poly_wrap_.inner);
        case EX_CAST:         return alloc_only(pu, e->as.cast_.expr);
        case EX_REINTERPRET:  return alloc_only(pu, e->as.reinterpret_.expr);
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (!alloc_only(pu, e->as.let_.bindings[i].init)) return false;
            return alloc_only(pu, e->as.let_.body);
        case EX_IF:
            return alloc_only(pu, e->as.if_.cond) && alloc_only(pu, e->as.if_.then_) &&
                   alloc_only(pu, e->as.if_.else_or_null);
        case EX_DO:          return alloc_only_n(pu, e->as.do_.items, e->as.do_.n);
        case EX_CONS_LIST:   return alloc_only_n(pu, e->as.cons_list_.items, e->as.cons_list_.n);
        case EX_MAKE_STRUCT: return alloc_only_n(pu, e->as.make_struct_.field_values, e->as.make_struct_.n_fields);
        case EX_SET:
            return e->as.set_.target && !e->as.set_.target->is_global && alloc_only(pu, e->as.set_.value);
        case EX_CALL:
            if (e->as.call_.fn_expr || e->as.call_.dict_arg) return false;
            if (ctor_call(e)) return alloc_only_n(pu, e->as.call_.args, e->as.call_.n_args);
            return fn_alloc_only(pu, e->as.call_.fn_binding) &&
                   alloc_only_n(pu, e->as.call_.args, e->as.call_.n_args);
        default:
            return false;
    }
}

static bool is_candidate(const Expr *e, bool keep_exported, Purity *pu) {
    if (e->kind != EX_FN_DEF && e->kind != EX_DEF) return false;
    if (!srfi_file_span(e->span)) return false;
    Binding *b = item_binding(e);
    if (!b) return false;
    if (b->retain_c_linkage || b->c_export_name) return false;
    if (keep_exported && b->is_exported) return false;
    if (e->kind == EX_FN_DEF) {
        const FnDef *fd = e->as.fn_def_.fn;
        if (fd->owner_instance || b->is_instance_method) return false;
    } else if (!init_has_no_effect(e->as.def_.init) && !alloc_only(pu, e->as.def_.init)) {
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * The walk
 * ------------------------------------------------------------------------- */
static void push(Pr *p, const Expr *e) {
    if (!e) return;
    if (p->sp == p->cap_stack) {
        p->cap_stack = p->cap_stack ? p->cap_stack * 2 : 1024;
        p->stack = (const Expr **)realloc((void *)p->stack, p->cap_stack * sizeof *p->stack);
    }
    p->stack[p->sp++] = e;
}

static void push_n(Pr *p, Expr *const *arr, uint32_t n) {
    if (!arr) return;
    for (uint32_t i = 0; i < n; i++) push(p, arr[i]);
}

static void reach_fn(Pr *p, const FnDef *fd);

/* A Binding the live code names.  Reaching one reaches the candidate that
 * defines it, and every function the emitter may spell in its place. */
static void reach(Pr *p, const Binding *b) {
    if (!b || !pset_put(&p->reached, b, 0)) return;
    uint32_t idx;
    if (pset_get(&p->cand_of, b, &idx) && !p->items[idx].live) {
        p->items[idx].live = true;
        p->items[idx].from = p->cur;
        p->items[idx].from_kind = p->cur_kind;
        p->items[idx].from_via = p->via;
        if (p->n_work == p->cap_work) {
            p->cap_work = p->cap_work ? p->cap_work * 2 : 256;
            p->work = (uint32_t *)realloc(p->work, p->cap_work * sizeof *p->work);
        }
        p->work[p->n_work++] = idx;
    }
    const Binding *saved_via = p->via;
    p->via = b;
    reach(p, b->closure_fn_binding);
    reach(p, b->hoist_closure_fn_binding);
    reach(p, b->returns_closure_fn_binding);
    reach(p, b->source_binding);
    reach(p, b->widen_fn_alias);
    if (b->source_fn_def) reach_fn(p, b->source_fn_def);
    push(p, b->deferred_init);
    push(p, b->closure_head_init);
    p->via = saved_via;
}

static void reach_all(Pr *p, Binding *const *bs, uint32_t n) {
    if (!bs) return;
    for (uint32_t i = 0; i < n; i++) reach(p, bs[i]);
}

/* A function (a defn, a lambda, an instance method): its name, its body and
 * what its closure captures. */
static void reach_fn(Pr *p, const FnDef *fd) {
    if (!fd) return;
    reach(p, fd->binding);
    if (!pset_put(&p->walked, fd, 0)) return;
    push(p, fd->body);
    if (fd->closure) reach_all(p, fd->closure->captures, fd->closure->n_captures);
}

static void reach_instance(Pr *p, const TypeClassInstance *inst) {
    if (!inst || !pset_put(&p->walked, inst, 0)) return;
    for (uint8_t i = 0; i < inst->n_method_impls; i++)
        if (inst->method_impls) reach_fn(p, inst->method_impls[i]);
}

static void reach_gen(Pr *p, const GenDef *g) {
    if (!g || !pset_put(&p->walked, g, 0)) return;
    push(p, g->body);
    reach_all(p, g->captures, g->n_captures);
}

static void reach_handle(Pr *p, const HandleExpr *h) {
    if (!h) return;
    push(p, h->body);
    for (uint8_t i = 0; i < h->n_cases; i++) push(p, h->cases[i].body);
}

/* One node: push its operands, reach the Bindings it names.  Every kind is
 * listed, with no default, so -Wswitch names a kind added later; the walk
 * gives up on one it does not recognise at run time as well. */
static void visit(Pr *p, const Expr *e) {
    p->cur_kind = (int)e->kind;
    switch (e->kind) {
        /* --- leaves ---------------------------------------------------- */
        case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT: case EX_FLOAT_LIT:
        case EX_CSTR_LIT: case EX_SYM_LIT: case EX_DEFAULT_OF: case EX_RETRY:
        case EX_EXTERN_C: case EX_TYPECLASS_DEF: case EX_DEFECT:
        case EX_DEFDATA: case EX_DEFGADT: case EX_DYNVAR_READ:
            return;
        case EX_DICT:
            reach_instance(p, e->as.dict_.instance);
            return;
        case EX_INSTANCE_DEF:
            reach_instance(p, e->as.instance_def_.instance);
            return;

        /* --- names ----------------------------------------------------- */
        case EX_VAR: reach(p, e->as.var.binding); return;
        case EX_SET: reach(p, e->as.set_.target); push(p, e->as.set_.value); return;
        case EX_DEF: push(p, e->as.def_.init); return;

        /* --- functions ------------------------------------------------- */
        case EX_FN:     reach_fn(p, e->as.fn_.fn);     return;
        case EX_FN_DEF: reach_fn(p, e->as.fn_def_.fn); return;
        case EX_CLOSURE:
            if (e->as.closure_.closure) {
                reach_fn(p, e->as.closure_.closure->fn);
                reach_all(p, e->as.closure_.closure->captures,
                          e->as.closure_.closure->n_captures);
            }
            return;
        case EX_INLINE_C:
            if (e->as.inline_c_.inline_c) {
                const InlineC *ic = e->as.inline_c_.inline_c;
                reach_all(p, ic->captures, ic->n_captures);
                push_n(p, ic->val_exprs, ic->n_val_exprs);
            }
            return;
        case EX_CALL:
            reach(p, e->as.call_.fn_binding);
            push(p, e->as.call_.fn_expr);
            push(p, e->as.call_.dict_arg);
            push_n(p, e->as.call_.args, e->as.call_.n_args);
            return;
        case EX_BUILTIN:   push_n(p, e->as.builtin.args, e->as.builtin.n);        return;
        case EX_CONS_LIST: push_n(p, e->as.cons_list_.items, e->as.cons_list_.n); return;
        case EX_POLY_WRAP:
            reach(p, e->as.poly_wrap_.wrapper_binding);
            reach(p, e->as.poly_wrap_.dict_clone_binding);
            push(p, e->as.poly_wrap_.inner);
            return;
        case EX_FN_TO_FAT:   push(p, e->as.fn_to_fat_.inner);   return;
        case EX_POLY_TO_FAT: push(p, e->as.poly_to_fat_.inner); return;

        /* --- binding and sequencing ------------------------------------ */
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++) push(p, e->as.let_.bindings[i].init);
            push(p, e->as.let_.body);
            return;
        case EX_IF:
            push(p, e->as.if_.cond); push(p, e->as.if_.then_); push(p, e->as.if_.else_or_null);
            return;
        case EX_DO:      push_n(p, e->as.do_.items, e->as.do_.n); return;
        case EX_PROGRAM: push_n(p, e->as.program.items, e->as.program.n); return;
        case EX_DEFMODULE:
            if (e->as.defmodule_.mod)
                push_n(p, e->as.defmodule_.mod->body, e->as.defmodule_.mod->n_body);
            return;
        case EX_WHILE:  push(p, e->as.while_.cond); push(p, e->as.while_.body); return;
        case EX_RETURN: push(p, e->as.return_.value); return;
        case EX_DEFER:
            push(p, e->as.defer_.body);
            reach_all(p, e->as.defer_.captures, e->as.defer_.n_captures);
            return;

        /* --- aggregates ------------------------------------------------ */
        case EX_MAKE_STRUCT:
            push_n(p, e->as.make_struct_.field_values, e->as.make_struct_.n_fields);
            return;
        case EX_GET_FIELD: push(p, e->as.get_field_.struct_expr); return;
        case EX_SET_FIELD:
            push(p, e->as.set_field_.receiver); push(p, e->as.set_field_.value);
            return;
        case EX_SET_LIT: push_n(p, e->as.set_lit_.items, e->as.set_lit_.n); return;
        case EX_MATCH:
            push(p, e->as.match_.scrutinee);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                push(p, e->as.match_.arms[i].guard);
                push(p, e->as.match_.arms[i].body);
            }
            return;

        /* --- effects and control --------------------------------------- */
        case EX_PERFORM:
            if (e->as.perform_.perform)
                push_n(p, e->as.perform_.perform->args, e->as.perform_.perform->n_args);
            return;
        case EX_HANDLE:      reach_handle(p, e->as.handle_.handle);      return;
        case EX_HANDLER_LIT: reach_handle(p, e->as.handler_lit_.handle); return;
        case EX_WITH_HANDLER:
            push(p, e->as.with_handler_.handler); push(p, e->as.with_handler_.body);
            return;
        case EX_COMPOSE_HANDLERS:
            push(p, e->as.compose_handlers_.h1); push(p, e->as.compose_handlers_.h2);
            return;
        case EX_RESUME:
            if (e->as.resume_.resume) {
                push(p, e->as.resume_.resume->k); push(p, e->as.resume_.resume->value);
            }
            return;
        case EX_DISCONTINUE:
            if (e->as.discontinue_.discontinue) {
                push(p, e->as.discontinue_.discontinue->k);
                push(p, e->as.discontinue_.discontinue->exception);
            }
            return;
        case EX_CONT_PRED:       push(p, e->as.cont_pred_.expr); return;
        case EX_RESET:           push(p, e->as.reset_.body); return;
        case EX_SHIFT:           push(p, e->as.shift_.k_fn); push(p, e->as.shift_.body); return;
        case EX_SHIFT0:          push(p, e->as.shift0_.k_fn); push(p, e->as.shift0_.body); return;
        case EX_CALLCC:          push(p, e->as.callcc_.fn); return;
        case EX_CLONEABLE_RESET: push(p, e->as.cloneable_reset_.body); return;
        case EX_CLONEABLE_SHIFT:
            push(p, e->as.cloneable_shift_.k_fn); push(p, e->as.cloneable_shift_.body);
            return;
        case EX_SERIAL_RESET: push(p, e->as.serial_reset_.body); return;
        case EX_SERIAL_SHIFT:
            push(p, e->as.serial_shift_.k_fn); push(p, e->as.serial_shift_.body);
            return;
        case EX_CPS_CONT_APP:
            push(p, e->as.cps_cont_app_.cont); push(p, e->as.cps_cont_app_.value);
            return;

        /* --- concurrency ----------------------------------------------- */
        case EX_ASYNC: push(p, e->as.async_.fn_expr);  return;
        case EX_AWAIT: push(p, e->as.await_.fut_expr); return;
        case EX_SELECT:
            for (uint32_t i = 0; i < e->as.select_.n_clauses; i++) {
                push(p, e->as.select_.clauses[i].chan);
                push(p, e->as.select_.clauses[i].send_val);
                push(p, e->as.select_.clauses[i].body);
            }
            push(p, e->as.select_.default_body);
            return;
        case EX_STM:        push_n(p, e->as.stm_.body, e->as.stm_.n_body); return;
        case EX_ATOMICALLY: push(p, e->as.atomically_.stm_expr); return;
        case EX_CHECK:      push(p, e->as.check_.cond); return;
        case EX_OR_ELSE:    push(p, e->as.or_else_.stm1); push(p, e->as.or_else_.stm2); return;
        case EX_TVAR_NEW:   push(p, e->as.tvar_new_.init); return;
        case EX_TVAR_READ:  push(p, e->as.tvar_read_.tvar); return;
        case EX_TVAR_WRITE: push(p, e->as.tvar_write_.tvar); push(p, e->as.tvar_write_.value); return;
        case EX_TVAR_MODIFY: push(p, e->as.tvar_modify_.tvar); push(p, e->as.tvar_modify_.fn); return;
        case EX_TVAR_SWAP:  push(p, e->as.tvar_swap_.tvar); push(p, e->as.tvar_swap_.new_val); return;
        case EX_TVAR_CAS:
            push(p, e->as.tvar_cas_.tvar); push(p, e->as.tvar_cas_.old_val);
            push(p, e->as.tvar_cas_.new_val);
            return;

        /* --- generators ------------------------------------------------ */
        case EX_GEN:        reach_gen(p, e->as.gen_.def); return;
        case EX_YIELD:      push(p, e->as.yield_.value); return;
        case EX_GEN_NEXT:
            push(p, e->as.gen_next_.gen_expr); reach_gen(p, e->as.gen_next_.def);
            return;
        case EX_GEN_DONE:   push(p, e->as.gen_done_.gen_expr); return;
        case EX_GEN_UNWRAP: push(p, e->as.gen_unwrap_.ptr_expr); return;

        /* --- dynamic vars ---------------------------------------------- */
        case EX_DEFDYNAMIC: push(p, e->as.defdynamic_.root_expr); return;
        case EX_DYNVAR_SET: push(p, e->as.dynvar_set_.value); return;
        case EX_DYNVAR_BINDING:
            for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++)
                push(p, e->as.dynvar_binding_.pairs[i].override_expr);
            push(p, e->as.dynvar_binding_.body);
            return;

        /* --- references, rc, borrows ----------------------------------- */
        case EX_REF:          push(p, e->as.ref_.expr); return;
        case EX_DEREF:        push(p, e->as.deref_.expr); return;
        case EX_SET_DEREF:    push(p, e->as.set_deref_.ref); push(p, e->as.set_deref_.value); return;
        case EX_RC_OF:        push(p, e->as.rc_of_.expr); return;
        case EX_RC_CLONE:     push(p, e->as.rc_clone_.expr); return;
        case EX_RC_DROP:      push(p, e->as.rc_drop_.expr); return;
        case EX_RC_PTR:       push(p, e->as.rc_ptr_.expr); return;
        case EX_RC_COUNT:     push(p, e->as.rc_count_.expr); return;
        case EX_RC_FROM_REF:  push(p, e->as.rc_from_ref_.expr); return;
        case EX_REF_FROM_RC:  push(p, e->as.ref_from_rc_.expr); return;
        case EX_WEAK:         push(p, e->as.weak_.expr); return;
        case EX_WEAK_UPGRADE: push(p, e->as.weak_upgrade_.expr); return;
        case EX_WEAK_PRED:    push(p, e->as.weak_pred_.expr); return;
        case EX_REF_PRED:     push(p, e->as.ref_pred_.expr); return;
        case EX_BORROW_IMMUT: push(p, e->as.borrow_immut_.expr); return;
        case EX_BORROW_MUT:   push(p, e->as.borrow_mut_.expr); return;

        /* --- panics ---------------------------------------------------- */
        case EX_PANIC:               push(p, e->as.panic_.payload); return;
        case EX_PANIC_WITH:          push(p, e->as.panic_with_.payload); return;
        case EX_CATCH_UNWIND:        push(p, e->as.catch_unwind_.thunk); return;
        case EX_CATCH_PANIC_OF:      push(p, e->as.catch_panic_of_.thunk); return;
        case EX_PANIC_PAYLOAD_TYPE:  push(p, e->as.panic_payload_type_.payload); return;
        case EX_PANIC_PAYLOAD_VALUE: push(p, e->as.panic_payload_value_.payload); return;
        case EX_PANIC_PAYLOAD_FILE:  push(p, e->as.panic_payload_file_.payload); return;
        case EX_PANIC_PAYLOAD_LINE:  push(p, e->as.panic_payload_line_.payload); return;
        case EX_PANIC_PAYLOAD_DOWNS: push(p, e->as.panic_payload_downs_.payload); return;

        /* --- representation wrappers ----------------------------------- */
        case EX_ASCRIBE:     push(p, e->as.ascribe_.inner); return;
        case EX_CAST:        push(p, e->as.cast_.expr); return;
        case EX_REINTERPRET: push(p, e->as.reinterpret_.expr); return;

        /* --- existentials ---------------------------------------------- */
        case EX_EXISTS_PACK: push(p, e->as.exists_pack_.value); return;
        case EX_EXISTS_OPEN:
            push(p, e->as.exists_open_.packed); push(p, e->as.exists_open_.body);
            return;
        case EX_EXISTS_DISPATCH:
            push_n(p, e->as.exists_dispatch_.args, e->as.exists_dispatch_.n_args);
            return;

        /* --- the dynamic surface --------------------------------------- */
        case EX_UNION_INJECT: push(p, e->as.union_inject_.value); return;
        case EX_ANY_TYPE_OF:  push(p, e->as.any_type_of_.value); return;
        case EX_ANY_CAST:     push(p, e->as.any_cast_.value); return;
        case EX_ANY_IS:       push(p, e->as.any_is_.value); return;
        case EX_DYN_OP:       push_n(p, e->as.dyn_op_.args, e->as.dyn_op_.n_args); return;
        case EX_DYN_CALL:
            push(p, e->as.dyn_call_.fn);
            push_n(p, e->as.dyn_call_.args, e->as.dyn_call_.n_args);
            return;
        case EX_DYN_FIELD: push(p, e->as.dyn_field_.obj); return;
        case EX_DYN_METHOD:
            push(p, e->as.dyn_method_.obj);
            push_n(p, e->as.dyn_method_.args, e->as.dyn_method_.n_args);
            return;
    }
    p->gave_up = true;
}

static void drain(Pr *p) {
    while (p->sp > 0 && !p->gave_up) visit(p, p->stack[--p->sp]);
}

static void walk_item(Pr *p, const Expr *e) {
    push(p, e);
    drain(p);
}

/* ---------------------------------------------------------------------------
 * The pass
 * ------------------------------------------------------------------------- */
static uint32_t rewrite(Arena *arena, Expr ***arr, uint32_t *n, const PSet *dead) {
    uint32_t kept = 0, dropped = 0;
    Expr **out = (Expr **)arena_alloc(arena, (*n ? *n : 1) * sizeof *out);
    for (uint32_t i = 0; i < *n; i++) {
        Expr *e = (*arr)[i];
        if (e && pset_get(dead, e, NULL)) { dropped++; continue; }
        if (e && e->kind == EX_DEFMODULE && e->as.defmodule_.mod) {
            DefModule *m = e->as.defmodule_.mod;
            dropped += rewrite(arena, &m->body, &m->n_body, dead);
        }
        out[kept++] = e;
    }
    *arr = out;
    *n = kept;
    return dropped;
}

uint32_t srfi_prune_program(Arena *arena, Expr *prog, bool keep_exported) {
    if (!prog || prog->kind != EX_PROGRAM) return 0;
    const char *off = getenv("TUR_NO_SRFI_PRUNE");
    if (off && *off && strcmp(off, "0") != 0) return 0;

    Pr p;
    memset(&p, 0, sizeof p);
    collect_items(&p, prog->as.program.items, prog->as.program.n);

    /* The stdlib/srfi/ procedures an allocation-only initializer may call. */
    Purity pu;
    memset(&pu, 0, sizeof pu);
    for (uint32_t i = 0; i < p.n_items; i++) {
        Expr *e = *p.items[i].slot;
        if (!e || e->kind != EX_FN_DEF || !e->as.fn_def_.fn || !srfi_file_span(e->span)) continue;
        const FnDef *fd = e->as.fn_def_.fn;
        if (!fd->binding || pset_get(&pu.fn_of, fd->binding, NULL)) continue;
        if (pu.n_fns == pu.cap_fns) {
            pu.cap_fns = pu.cap_fns ? pu.cap_fns * 2 : 256;
            pu.fns = (const FnDef **)realloc((void *)pu.fns, pu.cap_fns * sizeof *pu.fns);
        }
        pset_put(&pu.fn_of, fd->binding, pu.n_fns);
        pu.fns[pu.n_fns++] = fd;
    }

    /* Candidates.  A Binding two items define is left alone (both stay). */
    uint32_t n_cand = 0;
    PSet twice = {0};
    for (uint32_t i = 0; i < p.n_items; i++) {
        Expr *e = *p.items[i].slot;
        if (!is_candidate(e, keep_exported, &pu)) continue;
        Binding *b = item_binding(e);
        uint32_t prev;
        if (pset_get(&p.cand_of, b, &prev)) { pset_put(&twice, b, 0); continue; }
        pset_put(&p.cand_of, b, i);
        p.items[i].candidate = true;
        p.items[i].live = false;
        n_cand++;
    }
    for (uint32_t i = 0; i < p.n_items; i++) {
        if (!p.items[i].candidate) continue;
        if (pset_get(&twice, item_binding(*p.items[i].slot), NULL)) {
            p.items[i].candidate = false;
            p.items[i].live = true;
            n_cand--;
        }
    }
    pset_free(&twice);
    free((void *)pu.fns);
    pset_free(&pu.fn_of);
    pset_free(&pu.state);

    /* TUR_SRFI_PRUNE_DEBUG=1 says, on stderr, which SRFI definitions stayed
     * and what reached each one -- the question to ask when an unused import
     * still costs something. */
    const char *dbg = getenv("TUR_SRFI_PRUNE_DEBUG");
    bool debug = dbg && *dbg && strcmp(dbg, "0") != 0;

    uint32_t dropped = 0;
    if (n_cand > 0) {
        /* The roots: every item that is not a candidate. */
        for (uint32_t i = 0; i < p.n_items && !p.gave_up; i++)
            if (!p.items[i].candidate) { p.cur = i; walk_item(&p, *p.items[i].slot); }
        /* Then each candidate as it is reached, until none is left. */
        while (p.n_work > 0 && !p.gave_up) {
            uint32_t idx = p.work[--p.n_work];
            p.cur = idx;
            walk_item(&p, *p.items[idx].slot);
        }
        if (debug) {
            fprintf(stderr, "srfi-prune: %u candidates%s\n", n_cand,
                    p.gave_up ? "; the walk met a kind it does not model, so none is dropped" : "");
            for (uint32_t i = 0; i < p.n_items; i++) {
                if (!p.items[i].candidate || !p.items[i].live) continue;
                const Binding *b = item_binding(*p.items[i].slot);
                const Expr *fe = *p.items[p.items[i].from].slot;
                const Binding *fb = item_binding(fe);
                const SourceFile *ff = diag_source_file(fe->span.file_id);
                const Binding *via = p.items[i].from_via;
                fprintf(stderr, "srfi-prune: kept %s: named by an expression of kind %d%s%s,"
                                " walking %s (%s:%u)\n",
                        b && b->name ? b->name->name : "?", p.items[i].from_kind,
                        via ? " through the links of " : "",
                        via && via->name ? via->name->name : "",
                        fb && fb->name ? fb->name->name : "?",
                        ff && ff->path ? ff->path : "?", (unsigned)fe->span.line);
            }
        }
        if (!p.gave_up) {
            PSet dead = {0};
            for (uint32_t i = 0; i < p.n_items; i++)
                if (p.items[i].candidate && !p.items[i].live)
                    pset_put(&dead, *p.items[i].slot, 0);
            if (dead.n > 0)
                dropped = rewrite(arena, &prog->as.program.items, &prog->as.program.n, &dead);
            pset_free(&dead);
        }
    }

    free(p.items);
    free(p.work);
    free((void *)p.stack);
    pset_free(&p.cand_of);
    pset_free(&p.reached);
    pset_free(&p.walked);
    return dropped;
}
