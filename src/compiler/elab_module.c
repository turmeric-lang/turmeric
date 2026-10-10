/* elab_module.c -- module loading, imports, exports, and symbol resolution. */
#include "scheme_lower.h"
#include "runtime/globals.h"     /* g_lang_prelude */   /* r7rs-lang-plan R2: Scheme core forms in an imported module */
#include "elab_internal.h"
#include "lang_dialects.h"      /* lang_span_is_dynamic: the H6 forward-decl rule */

/* ---- file-local helper forward declarations ---- */
static bool module_name_valid(const char *name, uint32_t len);
static ElabModule *elab_load_module(Elab *e, const Symbol *name, Span import_span);
static bool parse_import_spec(Elab *e, const Form *f, ImportSpec *out);
static void elab_forward_declare_defns(Elab *e, Form *const *items,
                                       uint32_t start, uint32_t end);

/* Pass 1: forward-declare every top-level `defn` in items[start..end) into the
 * global scope, so mutually- and self-recursive functions in the range can see
 * each other before their bodies elaborate.  Mirrors the pre-pass the entry
 * unit runs in elaborate_program (elab_toplevel.c) -- extracted here so both
 * the defmodule body (elab_defmodule) and the top-level forms of an imported
 * module (elab_load_module, where a spliced `(load ...)` can drop a
 * self-recursive defn like typeclass-show.tur's `vec-show-loop`) get the same
 * forward declarations.  Without it, a self-recursive spliced defn's own
 * recursive call resolved to "unknown function or operator"; see
 * docs/archive/compiled-string-return-int-conversion.md (secondary blocker). */
static void elab_forward_declare_defns(Elab *e, Form *const *items,
                                       uint32_t start, uint32_t end) {
    for (uint32_t j = start; j < end; j++) {
        Form *f = items[j];
        if (f->tag != F_LIST || f->as.list.len == 0) continue;
        Form *h = f->as.list.items[0];
        if (h->tag == F_SYM && h->as.sym == e->sym_def) { elab_pre_declare_any_mut_def(e, f); continue; }
        if (h->tag != F_SYM || h->as.sym != e->sym_defn) continue;
        if (f->as.list.len < 3) continue;
        /* Skip every pre-name attribute -- #[no-unwind]/#[used], export-as,
         * and the ^attrs (^construct, ^deprecated, ^reflect, ...). */
        uint32_t name_idx = elab_defn_name_index(e, f);
        if ((uint32_t)f->as.list.len <= name_idx) continue;
        Form *fn_name_f = f->as.list.items[name_idx];
        if (fn_name_f->tag != F_SYM) continue;
        /* Check not already defined */
        if (scope_lookup(&e->global, fn_name_f->as.sym)) continue;
        /* SS3a / general: scan for return-type annotation in the defn
         * form to get the real result_kind for the forward declaration.
         * Without this, recursive functions declared :nil infer TY_INT
         * (the placeholder) and emit as int64_t.
         * params vector is at name_idx+1; return type annotation is
         * at name_idx+2 if it is F_KEYWORD or F_TYPE_ANN. */
        TypeKind fwd_result_kind = TY_INT; /* placeholder */
        Type *fwd_result_full = NULL;
        /* A GENERIC defn spells as `(defn f [TypeVars] [params] : R ...)`, or
         * with a constraint vec `[TypeVars] [(C V)] [params]`, so the vector
         * after the name is the TYPE parameters and the real params sit one or
         * two slots further on.  This pre-pass assumed `name_idx + 1` for both
         * the params and (via +2) the return type, so a generic defn inside a
         * `defmodule` was forward-declared with the arity of its TYPE-parameter
         * list and a placeholder return.  A caller written ABOVE it then saw a
         * one-argument function: "returns int, which is not callable -- did you
         * mean to pass all 1 argument(s)?".  Defining the callee first hid it,
         * which made it look like generics simply could not be forward
         * referenced.  elab_toplevel.c's twin grew this skip for the return type
         * (poly-defn-recursive-return-type-inference); the defmodule half never
         * had it, for either. */
        uint32_t params_idx = name_idx + 1;
        if ((uint32_t)f->as.list.len > params_idx + 1 &&
            f->as.list.items[params_idx]->tag == F_VEC &&
            f->as.list.items[params_idx + 1]->tag == F_VEC) {
            params_idx++;   /* past the type-param vec */
            if ((uint32_t)f->as.list.len > params_idx + 1 &&
                f->as.list.items[params_idx]->tag == F_VEC &&
                f->as.list.items[params_idx + 1]->tag == F_VEC) {
                params_idx++;   /* past the constraint vec */
            }
        }
        uint32_t ret_idx = params_idx + 1;
        /* Skip optional #{Unsafe} / effect-row annotation (F_MAP) */
        if (ret_idx < (uint32_t)f->as.list.len && f->as.list.items[ret_idx]->tag == F_MAP) {
            ret_idx++;
        }
        /* forward-call-to-aggregate-result-types-as-carrier: a named return
         * the shallow resolver cannot settle yet (it names a type this module
         * body defines further down), retried once that type registers. */
        const Form *pending_ret_f = NULL;
        if (ret_idx < (uint32_t)f->as.list.len) {
            Form *ret_f = f->as.list.items[ret_idx];
            /* Is this slot an annotation at all?  A `: T` is; so is a keyword
             * with a body after it.  A bare symbol or list is the BODY of an
             * unannotated defn, and must not be looked up as a type name. */
            bool ret_is_annotation = ret_f->tag == F_TYPE_ANN ||
                (ret_f->tag == F_KEYWORD && (uint32_t)f->as.list.len > ret_idx + 1);
            /* Accept spaced `: T` (an F_TYPE_ANN wrapping a single
             * symbol/keyword) by unwrapping to the inner form -- mirrors the
             * top-level pre-pass in elab_toplevel.c.  Without this, a
             * recursive defn whose return type is written `: ptr<void>` (or
             * any spaced scalar) falls through to the TY_INT placeholder, so
             * the recursive call site is typed `int` and the if-branch
             * unifier rejects the body.  See
             * docs/archive/history/recursion-return-type-widens-to-int-inside-defmodule.md */
            if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1 &&
                (ret_f->as.list.items[0]->tag == F_SYM ||
                 ret_f->as.list.items[0]->tag == F_KEYWORD)) {
                ret_f = ret_f->as.list.items[0];
            } else if (ret_f->tag == F_TYPE_ANN && ret_f->as.list.len == 1 &&
                       ret_f->as.list.items[0]->tag == F_NIL) {
                /* forward-referenced-nil-call-bound-to-auto-type: the reader
                 * parses a bare `nil` in type position as F_NIL, not F_SYM, so
                 * the SYM/KEYWORD unwrap above never sees it and `: nil` fell
                 * through to the TY_INT placeholder.  A sibling caller
                 * elaborated BEFORE the callee then typed the call `int`, and
                 * the statement-position emitter bound a void call into an
                 * `__auto_type` temp ("variable has incomplete type 'void'").
                 * `: void` was immune only because `void` is an ordinary
                 * symbol.  The top-level pre-pass (elab_toplevel.c) has always
                 * handled F_NIL here; this is the defmodule half. */
                fwd_result_kind = TY_NIL;
                ret_f = NULL;
            }
            if (ret_f && (ret_f->tag == F_KEYWORD || ret_f->tag == F_SYM)) {
                const char *kn = ret_f->as.sym->name;
                if (strcmp(kn, "int") == 0) fwd_result_kind = TY_INT;
                else if (strcmp(kn, "bool") == 0) fwd_result_kind = TY_BOOL;
                else if (strcmp(kn, "float") == 0) fwd_result_kind = TY_FLOAT;
                else if (strcmp(kn, "cstr") == 0) fwd_result_kind = TY_CSTR;
                else if (strcmp(kn, "nil") == 0
                      || strcmp(kn, "void") == 0) fwd_result_kind = TY_NIL;
                else if (strcmp(kn, "ptr") == 0
                      || strcmp(kn, "ptr<void>") == 0) fwd_result_kind = TY_PTR_VOID;
                /* r7rs-lang-plan R7: an annotated `: any` result, like the
                 * unannotated dynamic default below it. */
                else if (strcmp(kn, "any") == 0) fwd_result_kind = TY_ANY;
                /* forward-call-to-aggregate-result-types-as-carrier: a bare
                 * type name -- `: Box` for a registered defstruct / defdata /
                 * defopaque -- rides the forward decl as that ADT, as the
                 * top-level pre-pass's bare-adt-forward-decl-inference arm
                 * does.  It used to keep the TY_INT placeholder, so a caller
                 * written above the callee typed the call as the carrier. */
                else if (ret_is_annotation) {
                    fwd_result_full = elab_fwd_compound_result_type(
                        e, f, name_idx, params_idx, ret_f);
                    if (fwd_result_full) fwd_result_kind = fwd_result_full->kind;
                    else pending_ret_f = ret_f;
                }
            } else if (ret_f && ret_f->tag == F_TYPE_ANN && ret_f->as.list.len > 0) {
                /* Compound return type: peek at the head symbol */
                Form *head_f = ret_f->as.list.items[0];
                if (head_f->tag == F_SYM &&
                        strcmp(head_f->as.sym->name, "Session") == 0) {
                    fwd_result_kind = TY_SESSION;
                } else {
                    /* r7rs-lang-plan R3 made a closed application ride the
                     * forward decl in full in a dynamic file;
                     * forward-call-to-aggregate-result-types-as-carrier does
                     * it in every file.  The TY_INT placeholder it replaces
                     * typed a forward call to `(defn good [] : (Result Handle
                     * cstr) ...)` as the int64 carrier, and the caller tripped
                     * TUR-E0709 against its own declared aggregate return. */
                    fwd_result_full = elab_fwd_compound_result_type(
                        e, f, name_idx, params_idx, ret_f);
                    if (fwd_result_full) fwd_result_kind = TY_APP;
                    else if (head_f->tag == F_LIST) pending_ret_f = ret_f;
                }
            }
        }
        /* Count actual arity + scalar arg kinds from the params vector.
         * fwd_decl_scan_params skips `^`-prefixed markers (^fat/^mut/...) so
         * the arity is not over-stated -- counting `^fat` as a slot made a
         * sibling forward-reference call look under-saturated and synthesised
         * a bogus extra-arg PAP wrapper.  See
         * docs/archive/history/pap-defmodule-fat-fn-too-many-args.md */
        TypeKind *arg_kinds = NULL;
        uint32_t param_arity = (params_idx < (uint32_t)f->as.list.len)
            ? fwd_decl_scan_params(e->arena, f->as.list.items[params_idx], &arg_kinds)
            : 0;
        /* saffron-dynamic-surface-pass H6, the module half: an UNANNOTATED
         * return in a dynamic file is `any`, and this forward decl is what a
         * caller elaborated before the callee sees.  elaborate_program's
         * pre-pass (elab_toplevel.c) has the rule; this one did not, so the
         * R7RS prelude compiled when a Scheme program was the entry and not
         * when a Turmeric module imported a Scheme library -- `r7rs-exint-of__`
         * calls `r7rs-exact`, defined further down, and the call was typed by
         * the TY_INT placeholder and re-tagged as a pointer at the `any`
         * widen (untyped-forward-callee-result-retagged-as-pointer).
         * Annotation is decided structurally, as there: a `: T` or a keyword
         * followed by a body; a bare symbol or list here is the body. */
        {
            bool ret_annotated = false;
            if (ret_idx < (uint32_t)f->as.list.len) {
                const Form *rf = f->as.list.items[ret_idx];
                ret_annotated = rf->tag == F_TYPE_ANN ||
                    (rf->tag == F_KEYWORD && (uint32_t)f->as.list.len > ret_idx + 1);
            }
            if (!ret_annotated && lang_span_is_dynamic(f->span) &&
                !(fn_name_f->as.sym->len == 4 && memcmp(fn_name_f->as.sym->name, "main", 4) == 0 &&
                  param_arity == 0)) {
                fwd_result_kind = TY_ANY;
                fwd_result_full = NULL;
            }
        }
        /* r7rs-lang-plan R3: a compound parameter type in a dynamic file
         * rides the forward decl in full -- see elab_fwd_param_full_types.
         * This pre-pass is the one an imported `#lang r7rs` prelude goes
         * through, so without it `r7rs-equal?` compiled its vector arm as an
         * int unbox whenever the program was a module. */
        Type **fwd_arg_full = elab_fwd_param_full_types(
            e, e->arena, f, name_idx, params_idx, param_arity, arg_kinds);
        Type fn_type = type_fn(arg_kinds, param_arity, fwd_result_kind);
        /* r7rs-lang-plan R7: the rest shape, as the top-level pre-pass does. */
        if (fwd_arg_full) fn_type.as.fn.arg_full_types = fwd_arg_full;
        if (params_idx < (uint32_t)f->as.list.len)
            fwd_decl_apply_variadic(e, e->arena, &fn_type, f->as.list.items[params_idx]);
        if (fwd_result_full) fn_type.as.fn.result_full_type = fwd_result_full;
        Binding *b = binding_new(e, fn_name_f->as.sym, fn_type, false, true, f->span);
        scope_add(&e->global, b);
        if (pending_ret_f && fwd_result_kind == TY_INT && !fwd_result_full)
            elab_fwd_note_pending_result(e, b, f, name_idx, params_idx,
                                         pending_ret_f);
    }
}

/* defmodule-bare-toplevel-forms-silently-dropped: may this elaborated form
 * stand as a direct child of `(defmodule ...)`?
 *
 * The emitter never reads `mod->body` -- it works off the definitions each
 * form registered globally as a side effect of elaborating -- so anything
 * here that is NOT a definition is elaborated, diagnosed like live code, and
 * then silently discarded.  That was the worst of the three available
 * behaviours: rejecting would be fine, emitting would be fine, but
 * type-checking a form *as if* it were live and then deleting it means every
 * signal the compiler gives says "live code" except the one that matters.
 *
 * The test is on the ELABORATED expression, not the source form's head
 * symbol, and that is load-bearing: a user macro that expands to a `defn` has
 * an arbitrary head, so a head-symbol allowlist would reject it.  Going
 * through elaboration also means the allow-set is small, because the
 * registering forms have already collapsed -- `defmacro`, `defclass` and
 * `deftype` all return EX_NIL_LIT once their definition is recorded.
 *
 * The set below is the one measured across stdlib/, tests/fixtures/ and
 * examples/, plus EX_TYPECLASS_DEF / EX_INSTANCE_DEF which are definitions by
 * construction.  Two entries look like expressions and are deliberate:
 *
 *   EX_INLINE_C -- a bare ```c block at module top level supplies file-scope
 *                  C declarations (tests/fixtures/inline-c-file-scope-*).
 *   EX_DEFER    -- module-level `defer` is a real feature: it runs at process
 *                  exit (tests/fixtures/module-defer-basic).
 *
 * EX_NIL_LIT admits a bare `nil`, which is inert either way.
 *
 * EX_DO is a third: a macro that emits SEVERAL definitions wraps them in an
 * implicit `(do ...)` (e.g. `derive-json` expanding to two `definstance`
 * forms, or an ecs `defworld-box-helpers` expanding to three `defn`s), and
 * that wrapper is not itself a defmacro/defclass/deftype, so it does not
 * collapse to EX_NIL_LIT the way a single-definition macro does. Recursing
 * is sound for the same reason a single form is: elaborating the `do`
 * already registered every child definition globally, so a `do` of
 * definitions is exactly as live as its unwrapped children would be. A `do`
 * containing even one non-definition (an actual side-effecting call) still
 * rejects, because that call site is unreachable in codegen the same as
 * before. */
static bool module_body_form_is_definition(const Expr *be) {
    switch (be->kind) {
        case EX_NIL_LIT:        /* defmacro / defmacro* / defclass / deftype */
        case EX_DEF:            /* def / defopaque */
        case EX_FN_DEF:         /* defn */
        case EX_DEFDATA:        /* defdata / defstruct */
        case EX_DEFECT:         /* defeffect */
        case EX_EXTERN_C:       /* extern-c */
        case EX_TYPECLASS_DEF:  /* defclass (when it keeps its own kind) */
        case EX_INSTANCE_DEF:   /* definstance */
        case EX_INLINE_C:       /* bare ```c block -- file-scope C decls */
        case EX_DEFER:          /* module-level defer -- runs at exit */
            return true;
        case EX_DO:             /* macro expanding to several definitions */
            for (uint32_t i = 0; i < be->as.do_.n; i++) {
                if (!module_body_form_is_definition(be->as.do_.items[i]))
                    return false;
            }
            return true;
        default:
            return false;
    }
}

/* class-and-generic-in-an-instance-less-module.  The class and a constrained
 * generic over it sit in one module, the instances in the importer -- the
 * layout a spice takes, with the vocabulary in one module and the instances
 * beside the types.  The imported module is elaborated whole at the import,
 * before any importer instance exists, so the generic's tyvar-receiver
 * dispatch had no representative instance and reported "declares no instance
 * at all" (TUR-E0015) for a program that declares several.  Such a defn is
 * parked instead, and retried once an instance has registered. */
typedef struct NoInstPending {
    Form       *form;
    DefModule  *mod;
    bool        in_imported_module;
    bool        done;
} NoInstPending;

static void noinst_park(Elab *e, Form *f, DefModule *mod) {
    if (e->n_noinst_pending >= e->cap_noinst_pending) {
        uint32_t nc = e->cap_noinst_pending ? e->cap_noinst_pending * 2 : 4;
        NoInstPending *np = (NoInstPending *)realloc(
            e->noinst_pending, nc * sizeof(NoInstPending));
        if (!np) { fprintf(stderr, "tur: oom\n"); abort(); }
        e->noinst_pending = np;
        e->cap_noinst_pending = nc;
    }
    NoInstPending *p = &e->noinst_pending[e->n_noinst_pending++];
    p->form = f;
    p->mod = mod;
    p->in_imported_module = e->in_imported_module;
    p->done = false;
}

/* Append a retried definition to its module's body (arena arrays, sized
 * exactly, so grow by copy).  Emission walks mod->body, and generic bodies
 * are forward-declared, so the position at the end is immaterial. */
static void noinst_append_body(Elab *e, DefModule *mod, Expr *x) {
    Expr **nb = (Expr **)arena_alloc(e->arena,
                                     (mod->n_body + 1) * sizeof(Expr *));
    for (uint32_t i = 0; i < mod->n_body; i++) nb[i] = mod->body[i];
    nb[mod->n_body] = x;
    mod->body = nb;
    mod->n_body++;
}

bool elab_noinst_retry(Elab *e, bool final) {
    if (e->n_noinst_pending == 0 || e->noinst_retrying) return true;
    if (!final && e->typeclass_env.instances == e->noinst_seen_head) return true;
    e->noinst_retrying = true;
    e->noinst_seen_head = e->typeclass_env.instances;
    bool saved_has_defmodule = e->has_defmodule;
    const Symbol *saved_name = e->current_module_name;
    const DefModule *saved_mod = e->current_module;
    bool saved_imported = e->in_imported_module;
    const Form *saved_tl = e->toplevel_stmt;
    bool ok = true;
    /* Until no parked defn makes progress: one may be the callee another
     * waits on. */
    bool progress = true;
    while (progress) {
        progress = false;
        for (uint32_t i = 0; i < e->n_noinst_pending; i++) {
            NoInstPending *p = &e->noinst_pending[i];
            if (p->done) continue;
            e->has_defmodule = true;
            e->current_module = p->mod;
            e->current_module_name = p->mod->name;
            e->in_imported_module = p->in_imported_module;
            e->toplevel_stmt = p->form;
            uint32_t mark = e->n_file_scope_defs;
            if (!final) diag_push_capture();
            Expr *x = elab_form(e, p->form);
            uint32_t cerr = final ? 0 : diag_pop_capture();
            if (x && cerr == 0 && module_body_form_is_definition(x)) {
                noinst_append_body(e, p->mod, x);
                p->done = true;
                progress = true;
            } else if (final) {
                p->done = true;
                ok = false;
            } else {
                e->n_file_scope_defs = mark;
            }
        }
        if (final) break;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < e->n_noinst_pending; i++)
        if (!e->noinst_pending[i].done) e->noinst_pending[k++] = e->noinst_pending[i];
    e->n_noinst_pending = k;
    if (k == 0) {
        free(e->noinst_pending);
        e->noinst_pending = NULL;
        e->cap_noinst_pending = 0;
    }
    e->has_defmodule = saved_has_defmodule;
    e->current_module_name = saved_name;
    e->current_module = saved_mod;
    e->in_imported_module = saved_imported;
    e->toplevel_stmt = saved_tl;
    e->noinst_retrying = false;
    return ok;
}

/* The defmodule body loop's state for elaborating one body form out of the
 * loop's own position: a flushed defn, or a deferred one's second chance. */
typedef struct MdRetryCtx {
    Elab            *e;
    const DefModule *mod;
    Form *const     *forms;
    uint32_t         n;
    const Form      *saved_tl_stmt;
    Expr           **slot;
    bool            *deferred;
    bool            *any_deferred;
    bool            *had_error;
    FwdGenOrder     *fgo;
} MdRetryCtx;

/* The body loop's treatment of form `s` at position `pos`: speculative when
 * it is a defn and a `definstance` follows `pos` (symptom A).  A defmodule
 * body is its own file-scope statement list, so each form here is a
 * statement for the def-position check -- exactly as in elaborate_program's
 * Pass 2.  Without this, `e->toplevel_stmt` would still name the enclosing
 * `(defmodule ...)` form and every `def` in the module would be reported as
 * sitting inside a top-level expression. */
static void md_elab_slot(MdRetryCtx *c, uint32_t s, uint32_t pos) {
    Elab *e = c->e;
    Form *f = c->forms[s];
    /* A statement boundary: a defn an imported module parked may resolve now
     * that this body has registered an instance. */
    elab_noinst_retry(e, false);
    bool md_may_defer = false;
    uint32_t md_fsd_mark = e->n_file_scope_defs;
    if (c->deferred && f->tag == F_LIST && f->as.list.len > 0) {
        Form *bh = f->as.list.items[0];
        if (bh->tag == F_SYM && bh->as.sym == e->sym_defn) {
            for (uint32_t k = pos + 1; k < c->n; k++) {
                Form *lf = c->forms[k];
                if (lf->tag != F_LIST || lf->as.list.len == 0) continue;
                Form *lh = lf->as.list.items[0];
                if (lh->tag == F_SYM && lh->as.sym == e->sym_definstance) {
                    md_may_defer = true;
                    break;
                }
            }
        }
    }
    /* class-and-generic-in-an-instance-less-module: in an imported module a
     * defn that is not already speculative is attempted under a capture
     * frame, so a failure that is ONLY "no instance at all" can be parked.
     * Any other failure is elaborated again, uncaptured, to report it. */
    bool md_may_park = !md_may_defer && e->in_imported_module &&
        !e->separate_compilation && !e->noinst_retrying &&
        f->tag == F_LIST && f->as.list.len > 0 &&
        f->as.list.items[0]->tag == F_SYM &&
        f->as.list.items[0]->as.sym == e->sym_defn;
    uint32_t md_noinst_mark = e->noinst_failures;
    if (md_may_defer || md_may_park) diag_push_capture();
    e->toplevel_stmt = f;
    Expr *be = elab_form(e, f);
    e->toplevel_stmt = c->saved_tl_stmt;
    if (md_may_defer) {
        uint32_t md_cerr = diag_pop_capture();
        if (md_cerr > 0 || !be) {
            e->n_file_scope_defs = md_fsd_mark;
            c->deferred[s] = true;
            *c->any_deferred = true;
            return;
        }
    } else if (md_may_park) {
        uint32_t md_cerr = diag_pop_capture();
        if (md_cerr > 0 || !be) {
            e->n_file_scope_defs = md_fsd_mark;
            if (e->noinst_failures > md_noinst_mark) {
                noinst_park(e, f, (DefModule *)c->mod);
                fwd_gen_order_done(c->fgo, s);
                return;
            }
            e->toplevel_stmt = f;
            be = elab_form(e, f);
            e->toplevel_stmt = c->saved_tl_stmt;
        }
    }
    fwd_gen_order_done(c->fgo, s);
    if (!be) {
        *c->had_error = true;
        return;  /* keep going to surface more diagnostics */
    }
    if (!module_body_form_is_definition(be)) {
        diag_emit_with_code(DIAG_ERROR, f->span,
                            TUR_E0711_MODULE_TOPLEVEL_EXPR,
                            "expression at (defmodule %s ...) top level is "
                            "never evaluated",
                            c->mod->name->name);
        diag_emit(DIAG_NOTE, f->span,
                  "only definitions run here -- there is no module-level "
                  "side-effect position.  Move this into a function "
                  "(main, or one main calls); for a test suite, call the "
                  "block from the module's entry point");
        *c->had_error = true;
        return;
    }
    c->slot[s] = be;
}

/* Elaborate, ahead of position `pos`, every waiting defn `f` names -- each
 * one's own waiting names first. */
static void md_flush(MdRetryCtx *c, const Form *f, uint32_t pos) {
    uint32_t d;
    while ((d = fwd_gen_order_next_flush(c->fgo, f)) != UINT32_MAX) {
        fwd_gen_order_done(c->fgo, d);
        md_flush(c, c->forms[d], pos);
        md_elab_slot(c, d, pos);
    }
}

/* A speculative attempt at one deferred body form, under a capture frame:
 * kept when it elaborates cleanly and is a definition, rolled back otherwise. */
static bool md_probe_slot(void *vctx, uint32_t s) {
    MdRetryCtx *c = (MdRetryCtx *)vctx;
    Form *f = c->forms[s];
    elab_noinst_retry(c->e, false);
    uint32_t mark = c->e->n_file_scope_defs;
    diag_push_capture();
    c->e->toplevel_stmt = f;
    Expr *be = elab_form(c->e, f);
    c->e->toplevel_stmt = c->saved_tl_stmt;
    uint32_t cerr = diag_pop_capture();
    if (cerr == 0 && be && module_body_form_is_definition(be)) {
        c->slot[s] = be;
        return true;
    }
    c->e->n_file_scope_defs = mark;
    return false;
}

/* The second chance for one deferred body form: no capture frame, so a
 * still-failing body reports for real. */
static void md_retry_slot(void *vctx, uint32_t s) {
    MdRetryCtx *c = (MdRetryCtx *)vctx;
    Form *f = c->forms[s];
    elab_noinst_retry(c->e, false);
    /* class-and-generic-in-an-instance-less-module: the second chance of a
     * deferred defn in an imported module may still be waiting on the
     * importer's instances -- park it rather than report. */
    if (c->e->in_imported_module && !c->e->separate_compilation &&
        !c->e->noinst_retrying) {
        uint32_t mark = c->e->n_file_scope_defs;
        uint32_t nmark = c->e->noinst_failures;
        diag_push_capture();
        c->e->toplevel_stmt = f;
        Expr *pe = elab_form(c->e, f);
        c->e->toplevel_stmt = c->saved_tl_stmt;
        uint32_t cerr = diag_pop_capture();
        if (cerr == 0 && pe && module_body_form_is_definition(pe)) {
            c->slot[s] = pe;
            return;
        }
        c->e->n_file_scope_defs = mark;
        if (c->e->noinst_failures > nmark) {
            noinst_park(c->e, f, (DefModule *)c->mod);
            return;
        }
    }
    c->e->toplevel_stmt = f;
    Expr *be = elab_form(c->e, f);
    c->e->toplevel_stmt = c->saved_tl_stmt;
    if (!be) { *c->had_error = true; return; }
    if (!module_body_form_is_definition(be)) {
        diag_emit_with_code(DIAG_ERROR, f->span, TUR_E0711_MODULE_TOPLEVEL_EXPR,
                            "expression at (defmodule %s ...) top level "
                            "is never evaluated",
                            c->mod->name->name);
        *c->had_error = true;
        return;
    }
    c->slot[s] = be;
}

/* Phase M0: Module system */

/* Validate that a module name only contains [a-zA-Z0-9_\-/] */
static bool module_name_valid(const char *name, uint32_t len) {
    if (len == 0) return false;
    for (uint32_t i = 0; i < len; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/') {
            continue;
        }
        return false;
    }
    /* Must not start or end with '/' */
    if (name[0] == '/' || name[len - 1] == '/') return false;
    return true;
}

/* Phase M: (load "path") — source-file inclusion form.
 * Reads the file at the given path, elaborates all its top-level forms into
 * the current module's scope, and returns nil.  Uses the loaded_modules
 * registry (keyed by interned path) to prevent duplicate loads.
 *
 * Syntax: (load "relative/or/absolute/path.tur")
 */
/* Phase M: (load "path") — handled by the load-expansion preprocessor in
 * elaborate_program before the two-pass elab.  The preprocessor expands loads
 * at the compilation-unit top level AND descends into defmodule bodies (so a
 * load inside a module splices the loaded file's forms into the module scope;
 * see docs/archive/history/load-inside-defmodule-silently-loses-names.md).  Any
 * (load ...) that still reaches this point is in genuine expression position
 * (a defn/let/do body), where it cannot act as a compile-time include; emit a
 * hard error so the programmer knows to move it to the top level or a
 * defmodule body. */
Expr *elab_load(Elab *e, const Form *call) {
    diag_emit(DIAG_ERROR, call->span,
              "load is only valid at the top level or directly in a defmodule body; "
              "move it out of the enclosing defn/let body");
    return NULL;
}

/* Phase M2: Load and elaborate an imported module file.
 * Returns the registry entry (may have 0 exports on parse/elab failure).
 * Returns NULL only on fatal error (circular import or OOM). */
/* Defined with elab_module_resolve_path below: the `.tur`-then-`.scm` read. */
static int module_read_named(const char *dir, const char *name,
                             char *out, size_t cap, int *out_len,
                             char **src, size_t *src_len);

static ElabModule *elab_load_module(Elab *e, const Symbol *name, Span import_span) {
    /* SB2: Reject imports in sandboxed environments. */
    if (e->sandboxed) {
        diag_emit(DIAG_ERROR, import_span,
                  "import not allowed in sandboxed environment");
        return NULL;
    }

    /* Already in registry? */
    ElabModule *existing = elab_find_loaded_module(e, name);
    if (existing) {
        if (existing->is_loading) {
            diag_emit(DIAG_ERROR, import_span,
                      "circular import: module '%s' is already being loaded", name->name);
            return NULL;
        }
        return existing;
    }

    /* Reserve a registry slot first (for circular-import detection). */
    if (e->n_loaded_modules >= e->cap_loaded_modules) {
        e->cap_loaded_modules = e->cap_loaded_modules ? e->cap_loaded_modules * 2 : 8;
        e->loaded_modules = (ElabModule *)realloc(e->loaded_modules,
                             e->cap_loaded_modules * sizeof(ElabModule));
        if (!e->loaded_modules) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    uint32_t slot_idx = e->n_loaded_modules++;
    ElabModule *slot = &e->loaded_modules[slot_idx];
    slot->name = name;
    slot->exports = NULL;
    slot->n_exports = 0;
    slot->exported_macros = NULL;
    slot->n_exported_macros = 0;
    slot->exported_effects = NULL;
    slot->n_exported_effects = 0;
    slot->is_loading = true;

    /* Build file path: 'geom/vector' -> '{base_dir}/geom/vector.tur'
     * The '/' in module names maps directly to directory separators.
     * A `.scm` file answers the same name when no `.tur` does
     * (module_read_named): a Scheme `define-library` needs no `.tur`
     * spelling to be importable. */
    char path_buf[4096];
    const char *base = e->module_base_dir ? e->module_base_dir : ".";
    int plen = snprintf(path_buf, sizeof(path_buf), "%s/%s.tur", base, name->name);
    if (plen < 0 || (size_t)plen >= sizeof(path_buf)) {
        diag_emit(DIAG_ERROR, import_span,
                  "module path too long for '%s'", name->name);
        slot->is_loading = false;
        return NULL;
    }

    /* Read source file. */
    char *src_raw = NULL;
    size_t src_len = 0;
    if (module_read_named(base, name->name, path_buf, sizeof(path_buf), &plen,
                          &src_raw, &src_len) != 0) {
        /* SC0: collect every path we attempt so a final failure can list
         * the full search. `attempted` grows via snprintf; we leave headroom
         * for one more append and the trailing NUL so overflow truncates
         * cleanly rather than producing a torn message. */
        char attempted[8192];
        int alen = 0;
        int w = snprintf(attempted + alen, sizeof(attempted) - (size_t)alen,
                         "    %s    (importing file's directory)\n", path_buf);
        if (w > 0 && (size_t)w < sizeof(attempted) - (size_t)alen) alen += w;

        /* Fallback: try the stdlib directory (e.g. for `import turi/eval`). */
        bool found_in_stdlib = false;
        if (e->module_stdlib_dir) {
            char stdlib_path[4096];
            int splen = snprintf(stdlib_path, sizeof(stdlib_path), "%s/%s.tur",
                                 e->module_stdlib_dir, name->name);
            if (splen > 0 && (size_t)splen < sizeof(stdlib_path)) {
                if (module_read_named(e->module_stdlib_dir, name->name, stdlib_path,
                                      sizeof(stdlib_path), &splen, &src_raw, &src_len) == 0) {
                    memcpy(path_buf, stdlib_path, (size_t)splen + 1);
                    plen = splen;
                    found_in_stdlib = true;
                } else if (alen < (int)sizeof(attempted) - 256) {
                    int sw = snprintf(attempted + alen, sizeof(attempted) - (size_t)alen,
                                      "    %s    (stdlib)\n", stdlib_path);
                    if (sw > 0 && (size_t)sw < sizeof(attempted) - (size_t)alen) alen += sw;
                }
            }
        }
        if (!found_in_stdlib) {
            /* Fallback: try each -I include directory (spice paths). */
            bool found_in_includes = false;
            int  matched_ii = -1;
            for (int ii = 0; ii < e->n_module_include_dirs && !found_in_includes; ii++) {
                char inc_path[4096];
                int iplen = snprintf(inc_path, sizeof(inc_path), "%s/%s.tur",
                                     e->module_include_dirs[ii], name->name);
                if (iplen > 0 && (size_t)iplen < sizeof(inc_path)) {
                    if (module_read_named(e->module_include_dirs[ii], name->name, inc_path,
                                          sizeof(inc_path), &iplen, &src_raw, &src_len) == 0) {
                        memcpy(path_buf, inc_path, (size_t)iplen + 1);
                        plen = iplen;
                        found_in_includes = true;
                        matched_ii = ii;
                    } else if (alen < (int)sizeof(attempted) - 256) {
                        int iw = snprintf(attempted + alen, sizeof(attempted) - (size_t)alen,
                                          "    %s    (-I %s)\n", inc_path,
                                          e->module_include_dirs[ii]);
                        if (iw > 0 && (size_t)iw < sizeof(attempted) - (size_t)alen) alen += iw;
                    }
                }
            }
            if (found_in_includes && matched_ii >= 0
                && e->module_include_workspace_producer
                && e->module_include_workspace_producer[matched_ii]) {
                /* LS2 (local-spice-dev-workflow-plan): the import was
                 * satisfied by a workspace-sibling member's src/.  If the
                 * consumer doesn't declare that producer in its own
                 * :spices, emit a one-time warning so drift between
                 * imports and declared deps surfaces before release.
                 *
                 * Match producer by the basename of the sibling's
                 * member-relative dir (e.g. "spices/alpha" -> "alpha"),
                 * which matches the convention that :spices map keys
                 * use the producer's spice name. */
                const char *producer_path =
                    e->module_include_workspace_producer[matched_ii];
                const char *producer_basename = strrchr(producer_path, '/');
                producer_basename = producer_basename
                                    ? producer_basename + 1
                                    : producer_path;
                bool declared = false;
                for (int dj = 0; dj < e->n_module_consumer_declared_spices; dj++) {
                    if (strcmp(e->module_consumer_declared_spices[dj],
                               producer_basename) == 0) {
                        declared = true;
                        break;
                    }
                }
                if (!declared
                    && e->module_include_warned
                    && !e->module_include_warned[matched_ii]) {
                    e->module_include_warned[matched_ii] = true;
                    diag_emit(DIAG_WARNING, import_span,
                              "import '%s' resolved via workspace sibling "
                              "'%s'; declare it in :spices for release builds. "
                              "(set TUR_DEBUG_RESOLVER=1 for full resolver tracing)",
                              name->name, producer_path);
                }
            }
            if (!found_in_includes) {
                /* SC0: list every searched path and suggest a workaround.
                 * When no -I paths were passed, this is almost always an
                 * intra-spice import that needs the spice's src/ on the
                 * search path -- point at that explicitly. */
                /* NOTE: a malformed build.tur used to reach here as a cascade
                 * of `module not found` -- the manifest was discarded, the
                 * spice's src/ never joined the search path, and the -I src
                 * hint below actively sent readers away from the real cause.
                 * That no longer happens: TUR-E0624 (pkg_manifest_reassert)
                 * fails the compile before elaboration, so this diagnostic is
                 * reached only when the manifest is fine and the import really
                 * is unresolvable.  See
                 * docs/archive/manifest-read-failure-degrades-to-module-not-found.md. */
                const char *hint;
                if (e->n_module_include_dirs > 0) {
                    hint = "  hint: check the -I paths you passed, "
                           "or run `tur build <spice-src-dir>` to compile the whole spice";
                } else {
                    hint = "  hint: this looks like an intra-spice import.\n"
                           "        try `tur check -I src <file>` from the spice root,\n"
                           "        or build the whole spice with `tur build src/`";
                }
                /* r7rs-library-file-shape-and-export-rename: a Scheme
                 * `(import (two a))` reaches here as the module `two/a`, and
                 * nothing above says the library's NAME is where it is looked
                 * for -- a `(define-library (two a) ...)` in `lib.tur` is
                 * never found.  Say so, in the library's own spelling. */
                char r7note[512] = "";
                const SourceFile *isf = diag_source_file(import_span.file_id);
                if (isf && isf->lang == LANG_R7RS && isf->src &&
                    import_span.off_end > import_span.off_start && import_span.off_end <= isf->len) {
                    size_t sl = import_span.off_end - import_span.off_start;
                    const char *st = isf->src + import_span.off_start;
                    bool turmeric_ns = false;
                    for (size_t k = 0; k + 10 <= sl && !turmeric_ns; k++)
                        turmeric_ns = memcmp(st + k, "(turmeric ", 10) == 0;
                    if (!turmeric_ns) {
                        char lib[256]; size_t at = 0;
                        for (const char *p = name->name; *p && at + 2 < sizeof lib; p++)
                            lib[at++] = *p == '/' ? ' ' : *p;
                        lib[at] = '\0';
                        snprintf(r7note, sizeof r7note,
                                 "\n  note: a library is found by its name: (%s) must be the file %s.tur "
                                 "(or %s.scm) on the paths above, holding that one define-library "
                                 "-- a file holds one library, named after the file",
                                 lib, name->name, name->name);
                    }
                }
                diag_emit(DIAG_ERROR, import_span,
                          "module '%s' not found\n  searched:\n%s%s%s",
                          name->name, attempted, hint, r7note);
                slot->is_loading = false;
                return NULL;
            }
        }
    }

    /* Copy source into the arena so it outlives the load. */
    char *src_copy = (char *)arena_alloc(e->arena, src_len + 1);
    memcpy(src_copy, src_raw, src_len);
    src_copy[src_len] = '\0';
    free(src_raw);

    /* Register the source file for diagnostics.  Path must also live in arena. */
    char *path_copy = (char *)arena_alloc(e->arena, (size_t)plen + 1);
    memcpy(path_copy, path_buf, (size_t)plen + 1);

    SourceFile *sfile = (SourceFile *)arena_alloc(e->arena, sizeof(SourceFile));
    *sfile = (SourceFile){0};
    sfile->path = path_copy;
    sfile->src = src_copy;
    sfile->len = src_len;
    sfile->file_id = e->next_import_file_id++;
    sfile->reader_type = READER_TURMERIC;

    /* saffron-lang-plan S7: honour a `#lang` directive in an IMPORTED module.
     *
     * This path hardcoded READER_TURMERIC and never ran detection, so the
     * directive reached the reader as source and
     * `(import dyn)` on a `#lang saffron` module was
     * "unexpected character '#' (0x23)" -- a Saffron module could not be
     * imported at ALL.  D5's reverse direction ("a Turmeric module importing a
     * Saffron module sees `any`-typed exports ... nothing new is needed")
     * assumed the import worked; it did not.
     *
     * The `(load ...)` path in elab_toplevel.c already does exactly this, which
     * is why loading a Saffron file worked while importing one did not -- the
     * same split that let `tur fmt` reject every `#lang` file while every other
     * entry point accepted them.  Mirrors that path: the extension still wins
     * for the base reader, the directive supplies it otherwise, and the
     * LANGUAGE axis rides out beside it so the module elaborates as Saffron. */
    {
        ReaderType ext_type = reader_type_from_extension(path_copy);
        if (ext_type == READER_UNKNOWN) ext_type = READER_TURMERIC;
        const char  *msrc = src_copy;
        size_t       mlen = src_len;
        const char  *bad = NULL;
        size_t       bad_len = 0;
        LangDialect dialect = LANG_TURMERIC;
        ReaderType lang_type = detect_lang_dialect(src_copy, src_len,
                                                   &msrc, &mlen,
                                                   &bad, &bad_len,
                                                   &dialect);
        if (bad) {
            if (lang_type == READER_UNKNOWN)
                diag_emit(DIAG_ERROR, SPAN_UNKNOWN,
                          "unknown #lang base '%.*s' -- see `tur dialects` for the valid bases (in imported module '%s') (TUR-E0331)",
                          (int)bad_len, bad, path_copy);
            else
                diag_emit(DIAG_ERROR, SPAN_UNKNOWN,
                          "`#lang` takes a single base dialect; unexpected "
                          "trailing token '%.*s' in imported module '%s' "
                          "(TUR-E0330)", (int)bad_len, bad, path_copy);
            return false;
        }
        ReaderType chosen = (ext_type != READER_TURMERIC) ? ext_type : lang_type;
        if (!reader_type_is_implemented(chosen)) {
            diag_emit(DIAG_ERROR, SPAN_UNKNOWN,
                      "#lang %s in imported module '%s' is not yet implemented",
                      reader_type_name(chosen), path_copy);
            return false;
        }
        /* `.scm` names the Scheme language as well as its reader. */
        {
            LangDialect ext_dialect = lang_dialect_from_extension(path_copy);
            if (ext_dialect != LANG_TURMERIC) dialect = ext_dialect;
        }
        sfile->src         = msrc;
        sfile->len         = mlen;
        sfile->head_offset = (size_t)(msrc - src_copy);
        sfile->reader_type = chosen;
        sfile->lang        = dialect;
    }
    diag_register_file(sfile);
    diag_set_file_origin(sfile->file_id, import_span);

    /* Parse the source into forms.
     *
     * Transitive-RM (T1): pass the shared `user_macros` registry so the
     * imported file sees the same user macros the entry file did. The
     * registry can be NULL (legacy callers / no manifest) -- in that
     * case read_all_with_registry behaves identically to read_all.
     *
     * Loading semantics: see docs/reader-macros-plan.md ("Loading
     * semantics"). `(import ...)` and `(load ...)` (elab_toplevel.c)
     * both populate this same registry; macros declared in an imported
     * module become visible to anything loaded *after* it in the
     * compile's read order. */
    uint32_t nforms = 0;
    bool had_error_before = diag_had_error();
    Form **forms = read_all_with_registry(e->arena, e->st, sfile,
                                          e->user_macros, &nforms);
    if (forms) {
        /* load-not-expanded-in-imported-or-project-modules: expand this
         * module's top-level (load "path") forms before elaboration, exactly as
         * the entry unit does. Without this a `(load ...)` at column 1 of an
         * imported file survives to elab_load and errors "load is only valid at
         * the top level". The visited set is shared with the entry, so a path
         * loaded by both is spliced once. */
        Form **expanded = NULL;
        uint32_t n_expanded = 0;
        int lrc = elab_expand_module_loads(e, e->arena, e->st, forms, nforms,
                                           &expanded, &n_expanded);
        if (lrc != 0) {
            slot->is_loading = false;
            if (!had_error_before && diag_had_error()) {
                diag_emit(DIAG_NOTE, import_span,
                          "while loading module '%s'", name->name);
            }
            return NULL;
        }
        forms = expanded;
        nforms = n_expanded;
        /* r7rs-lang-plan R2: an imported `#lang r7rs` module gets the same
         * Scheme-form lowering the entry program gets, at the same point --
         * after its loads are spliced, before anything is elaborated. */
        if (scheme_lower_needed(forms, nforms)) {
            uint32_t lowered_n = 0;
            forms  = scheme_lower_program(e->arena, e->st, forms, nforms, &lowered_n,
                                          elab_scheme_library_path, NULL,
                                          elab_scheme_stdlib_file, e);
            nforms = lowered_n;
            /* R3 / D9, the reverse direction: a Turmeric program importing a
             * Scheme library.  The R7RS prelude is autoloaded only when the
             * ENTRY file is Scheme, so bring it in here through the same
             * `(load ...)` path a stdlib load takes -- once per compile (the
             * load-visited set dedups), before the library's own forms, and
             * as forms of the prelude's own file, so the library's defmodule
             * is still the first form of ITS file. */
            if (!(g_lang_prelude && strcmp(g_lang_prelude, "r7rs/prelude.tur") == 0)) {
                Form **ld_items = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
                ld_items[0] = form_sym(e->arena, import_span, symtab_intern(e->st, strslice("load", 4)));
                ld_items[1] = form_str(e->arena, import_span, "stdlib/r7rs/prelude.tur", 23);
                Form *ld = form_list(e->arena, import_span, ld_items, 2);
                Form *const one[1] = { ld };
                Form **pre = NULL; uint32_t npre = 0;
                if (elab_expand_module_loads(e, e->arena, e->st, one, 1, &pre, &npre) == 0 && npre > 0) {
                    Form **joined = (Form **)arena_alloc(e->arena, (npre + nforms + 1) * sizeof(Form *));
                    for (uint32_t i = 0; i < npre; i++) joined[i] = pre[i];
                    for (uint32_t i = 0; i < nforms; i++) joined[npre + i] = forms[i];
                    joined[npre + nforms] = NULL;
                    forms = joined;
                    nforms = npre + nforms;
                }
            }
        }
    }
    if (!forms) {
        slot->is_loading = false;
        /* Transitive-RM (T1) decision #4: if the sub-read introduced a
         * new error, attach a `while loading module X` note at the
         * import site so the user can trace the breadcrumb back from
         * (say) an "unexpected character '#'" inside `lib/syntax.tur`
         * to the `(import lib/syntax)` that triggered the load. */
        if (!had_error_before && diag_had_error()) {
            diag_emit(DIAG_NOTE, import_span,
                      "while loading module '%s'", name->name);
        }
        return NULL;
    }

    /* Elaborate the imported module's forms.
     * Save and restore fields that track the current defmodule context so the
     * outer caller's state is not corrupted. */
    bool         saved_has_defmodule   = e->has_defmodule;
    const Symbol *saved_module_name    = e->current_module_name;
    const DefModule *saved_module      = e->current_module;
    e->has_defmodule       = false;
    e->current_module_name = NULL;
    e->current_module      = NULL;
    /* load-not-expanded-in-imported-or-project-modules: mark that the forms
     * below belong to an imported module, so self-registering forms
     * (defclass/definstance/method defs) do not register themselves for
     * emission in the importer's TU under separate compilation. */
    bool saved_in_imported_module = e->in_imported_module;
    e->in_imported_module = true;

    /* Phase M4: capture the DefModule so we can check its export list for macros. */
    const DefModule *loaded_defmod = NULL;

    /* Pass 1: forward-declare the imported module's *top-level* defns before
     * elaborating any body.  A `(load ...)` spliced into this file (e.g.
     * `(load "stdlib/string.tur")`, which transitively splices
     * typeclass-show.tur) can drop a self- or mutually-recursive top-level
     * defn like `vec-show-loop`; without this pre-pass its own recursive call
     * resolves to "unknown function or operator".  The defmodule form itself is
     * elaborated with its own inner Pass 1 (elab_defmodule), so this only
     * matters for the bare top-level defns that live alongside it.  See
     * docs/archive/compiled-string-return-int-conversion.md (secondary
     * blocker). */
    elab_forward_declare_defns(e, forms, 0, nforms);

    const Form *saved_import_tl_stmt = e->toplevel_stmt;
    for (uint32_t i = 0; i < nforms; i++) {
        /* An imported module's forms are its own file-scope statement list, so
         * each is a statement for the def-position check.  Left unset, a plain
         * top-level `(def ...)` in an imported file (stdlib/math.tur) would be
         * measured against the IMPORTER's current form and rejected. */
        e->toplevel_stmt = forms[i];
        Expr *ex = elab_form(e, forms[i]);
        e->toplevel_stmt = saved_import_tl_stmt;
        if (!ex) {
            e->has_defmodule       = saved_has_defmodule;
            e->current_module_name = saved_module_name;
            e->current_module      = saved_module;
            e->in_imported_module  = saved_in_imported_module;
            slot = &e->loaded_modules[slot_idx];
            slot->is_loading = false;
            return NULL;
        }
        /* Register EX_DEFMODULE into file_scope_defs so its body gets emitted.
         * emit.c's flatten_program_items expands these into top-level C items.
         * Phase M3: Skip inlining when compiling each module separately; the
         * implementation file #includes the imported module's header instead. */
        if (ex->kind == EX_DEFMODULE) {
            loaded_defmod = ex->as.defmodule_.mod; /* Phase M4 */
            if (!e->separate_compilation) elab_register_file_def(e, ex);
        } else if (ex->kind != EX_NIL_LIT && !e->separate_compilation) {
            /* load-not-expanded-in-imported-or-project-modules: a top-level
             * (load ...) spliced bare definitions (e.g. arrow.tur's `>>>`)
             * ahead of this file's defmodule. The entry path emits every such
             * returned expr via items[]; mirror that here by registering it for
             * file-scope emission, otherwise the spliced defns elaborate into
             * scope but never reach codegen -> link errors. Self-registering
             * forms (defclass/definstance/nested defns) already returned nil and
             * are skipped by the EX_NIL guard. */
            elab_register_file_def(e, ex);
        }
    }

    e->has_defmodule       = saved_has_defmodule;
    e->current_module_name = saved_module_name;
    e->current_module      = saved_module;
    e->in_imported_module  = saved_in_imported_module;

    /* Recursive imports during elab_form() can grow e->loaded_modules and
     * invalidate the earlier `slot` pointer. Re-acquire the reserved slot
     * by index before writing the collected exports back into it. */
    slot = &e->loaded_modules[slot_idx];

    /* Collect exported bindings: those in the global scope owned by this
     * module, plus any this module re-exports via (export-from ...).  A
     * re-exported entry is the DEFINING module's Binding, not a copy -- the
     * consumer resolves to the same mangled symbol it would have reached by
     * importing the defining module directly, so re-export costs no wrapper
     * and no indirection. */
    uint32_t n_reexp = (loaded_defmod != NULL) ? loaded_defmod->n_reexports : 0;
    uint32_t n_exp = 0;
    for (uint32_t i = 0; i < e->global.n; i++) {
        Binding *b = e->global.bindings[i];
        if (b->defining_module_name == name && b->is_exported) n_exp++;
    }
    Binding **exp_arr = (n_exp == 0 && n_reexp == 0) ? NULL :
        (Binding **)arena_alloc(e->arena, (n_exp + n_reexp) * sizeof(Binding *));
    uint32_t idx = 0;
    for (uint32_t i = 0; i < e->global.n; i++) {
        Binding *b = e->global.bindings[i];
        if (b->defining_module_name == name && b->is_exported)
            exp_arr[idx++] = b;
    }
    for (uint32_t i = 0; i < n_reexp; i++) {
        /* Already validated in elab_defmodule, which proved the source module
         * EXPORTS this name.  Deliberately not re-checking defining_module_name
         * against the source here: in a chain (low -> mid -> hi) the binding hi
         * re-exports from mid was defined by low, so that check would break the
         * second hop.  A name with no binding is a re-exported MACRO, collected
         * by the macro pass below instead. */
        Binding *rb = scope_lookup(&e->global, loaded_defmod->reexports[i]);
        if (rb && rb->is_exported)
            exp_arr[idx++] = rb;
    }

    slot->exports   = exp_arr;
    slot->n_exports = idx;

    /* Phase M4: Collect exported macros from this module.
     * A macro is exported if its defining_module_name matches this module's name
     * AND its name appears in the module's (export ...) list. */
    slot->exported_macros   = NULL;
    slot->n_exported_macros = 0;
    if (loaded_defmod != NULL
        && (loaded_defmod->n_exports > 0 || loaded_defmod->n_reexports > 0)) {
        /* A macro qualifies either by being defined here and named in the
         * export list, or by being named in an (export-from src ...) whose
         * src defines it -- the re-export case forwards the MacroDef itself,
         * so an importer expands the original definition. */
        uint32_t n_mexp = 0;
        for (uint32_t i = 0; i < e->n_macros; i++) {
            MacroDef *m = e->macros[i];
            if (m->defining_module_name == name) {
                for (uint32_t j = 0; j < loaded_defmod->n_exports; j++) {
                    if (loaded_defmod->exports[j] == m->name) { n_mexp++; break; }
                }
                continue;
            }
            for (uint32_t j = 0; j < loaded_defmod->n_reexports; j++) {
                if (loaded_defmod->reexports[j] == m->name) { n_mexp++; break; }
            }
        }
        if (n_mexp > 0) {
            struct MacroDef **mexp_arr =
                (struct MacroDef **)arena_alloc(e->arena, n_mexp * sizeof(struct MacroDef *));
            uint32_t midx = 0;
            for (uint32_t i = 0; i < e->n_macros; i++) {
                MacroDef *m = e->macros[i];
                if (m->defining_module_name == name) {
                    for (uint32_t j = 0; j < loaded_defmod->n_exports; j++) {
                        if (loaded_defmod->exports[j] == m->name) {
                            mexp_arr[midx++] = m; break;
                        }
                    }
                    continue;
                }
                for (uint32_t j = 0; j < loaded_defmod->n_reexports; j++) {
                    if (loaded_defmod->reexports[j] == m->name) {
                        mexp_arr[midx++] = m; break;
                    }
                }
            }
            slot->exported_macros   = mexp_arr;
            slot->n_exported_macros = midx;
        }
    }

    /* PR5-3-D: Collect exported effects for this module. */
    {
        uint32_t n_eeff = 0;
        for (uint32_t i = 0; i < e->effect_env->n_effects; i++) {
            Effect *eff = e->effect_env->effects[i];
            if (eff->defining_module_name == name && eff->is_exported) n_eeff++;
        }
        Effect **eeff_arr = (n_eeff == 0) ? NULL :
            (Effect **)arena_alloc(e->arena, n_eeff * sizeof(Effect *));
        uint32_t eidx = 0;
        for (uint32_t i = 0; i < e->effect_env->n_effects; i++) {
            Effect *eff = e->effect_env->effects[i];
            if (eff->defining_module_name == name && eff->is_exported)
                eeff_arr[eidx++] = eff;
        }
        slot->exported_effects   = eeff_arr;
        slot->n_exported_effects = n_eeff;
    }

    slot->is_loading = false;
    return slot;
}

/* Parse a single (import module-name [:as alias] [:refer [syms...]]) form */
static bool parse_import_spec(Elab *e, const Form *f, ImportSpec *out) {
    if (f->tag != F_LIST || f->as.list.len < 2) {
        diag_emit(DIAG_ERROR, f->span,
                  "import requires a module name: (import module-name [:as alias] [:refer [syms...]])");
        return false;
    }
    Form *head = f->as.list.items[0];
    if (head->tag != F_SYM || head->as.sym != e->sym_import) {
        diag_emit(DIAG_ERROR, f->span, "expected (import ...)");
        return false;
    }
    Form *name_f = f->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "import module name must be a symbol");
        return false;
    }
    if (!module_name_valid(name_f->as.sym->name, name_f->as.sym->len)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "invalid module name '%s': only alphanumeric, '-', '_', '/' allowed",
                  name_f->as.sym->name);
        return false;
    }
    out->module_name = name_f->as.sym;
    out->alias = NULL;
    out->refer_syms = NULL;
    out->n_refer = 0;
    out->refer_effect_syms = NULL;
    out->n_refer_effects   = 0;
    out->for_macros = false;
    out->span = f->span;

    uint32_t i = 2;
    while (i < f->as.list.len) {
        Form *kw = f->as.list.items[i];
        if (kw->tag == F_KEYWORD && kw->as.sym == e->kw_as) {
            i++;
            if (i >= f->as.list.len) {
                diag_emit(DIAG_ERROR, kw->span, ":as requires an alias symbol");
                return false;
            }
            Form *alias_f = f->as.list.items[i];
            if (alias_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, alias_f->span, ":as alias must be a symbol");
                return false;
            }
            out->alias = alias_f->as.sym;
            i++;
        } else if (kw->tag == F_KEYWORD && kw->as.sym == e->kw_refer) {
            i++;
            if (i >= f->as.list.len) {
                diag_emit(DIAG_ERROR, kw->span, ":refer requires a vector of symbols");
                return false;
            }
            Form *refer_f = f->as.list.items[i];
            if (refer_f->tag != F_VEC) {
                diag_emit(DIAG_ERROR, refer_f->span, ":refer requires a vector [sym1 sym2 ...]");
                return false;
            }
            uint32_t n = refer_f->as.list.len;
            /* Count plain symbols vs (effect Name) entries. */
            uint32_t n_syms = 0, n_effs = 0;
            for (uint32_t j = 0; j < n; j++) {
                Form *sf = refer_f->as.list.items[j];
                if (sf->tag == F_SYM) {
                    n_syms++;
                } else if (sf->tag == F_LIST && sf->as.list.len == 2
                           && sf->as.list.items[0]->tag == F_SYM
                           && sf->as.list.items[0]->as.sym == e->sym_effect) {
                    n_effs++;
                } else {
                    diag_emit(DIAG_ERROR, sf->span,
                              ":refer list must contain symbols or (effect Name) forms");
                    return false;
                }
            }
            const Symbol **syms = (n_syms == 0) ? NULL :
                (const Symbol **)arena_alloc(e->arena, n_syms * sizeof(Symbol *));
            const Symbol **esyms = (n_effs == 0) ? NULL :
                (const Symbol **)arena_alloc(e->arena, n_effs * sizeof(Symbol *));
            uint32_t si = 0, ei = 0;
            for (uint32_t j = 0; j < n; j++) {
                Form *sf = refer_f->as.list.items[j];
                if (sf->tag == F_SYM) {
                    syms[si++] = sf->as.sym;
                } else {
                    /* (effect Name) -- already validated above */
                    Form *en = sf->as.list.items[1];
                    if (en->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, en->span, "effect name in :refer must be a symbol");
                        return false;
                    }
                    esyms[ei++] = en->as.sym;
                }
            }
            out->refer_syms        = syms;
            out->n_refer           = n_syms;
            out->refer_effect_syms = esyms;
            out->n_refer_effects   = n_effs;
            i++;
        } else if (kw->tag == F_KEYWORD && kw->as.sym == e->kw_for_macros) {
            /* Stage 3 (macro-system-direction-plan): macro-time-only import.
             * Takes no argument. */
            out->for_macros = true;
            i++;
        } else {
            diag_emit(DIAG_ERROR, kw->span,
                      "unexpected token in import; expected :as, :refer, or :for-macros");
            return false;
        }
    }
    if (out->for_macros && (out->alias || out->n_refer > 0 || out->n_refer_effects > 0)) {
        diag_emit(DIAG_ERROR, out->span,
                  ":for-macros is a macro-time-only import and cannot combine "
                  "with :as or :refer; a module needed in both phases is "
                  "imported twice -- (import %s) and (import %s :for-macros)",
                  out->module_name->name, out->module_name->name);
        return false;
    }
    return true;
}

/* Stage 3: resolve a module name to its file path, mirroring exactly the
 * search order elab_load_module walks below (importing file's directory ->
 * stdlib dir -> each -I include dir), but probing for existence only.
 * Keep the two in lockstep: a module resolvable by one must be resolvable
 * by the other. */
static bool module_path_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* The spellings a module name resolves to under a directory, in order: the
 * `.tur` file, then the `.scm` file (a Scheme `define-library`, r7rs-lang-plan
 * open question 4).  Each formats "<dir>/<name><ext>" into `out`. */
static const char *const MODULE_FILE_EXTS[] = { ".tur", ".scm" };
#define MODULE_FILE_EXT_COUNT 2

/* Read the module `name` under `dir` by either spelling.  On success `out`
 * holds the path read, `*out_len` its length, and the source is in
 * `*src`/`*src_len`; returns 0.  On failure `out` holds the `.tur` spelling
 * (what a "not found" message names) and returns -1. */
static int module_read_named(const char *dir, const char *name,
                             char *out, size_t cap, int *out_len,
                             char **src, size_t *src_len) {
    for (int k = 0; k < MODULE_FILE_EXT_COUNT; k++) {
        int n = snprintf(out, cap, "%s/%s%s", dir, name, MODULE_FILE_EXTS[k]);
        if (n <= 0 || (size_t)n >= cap) continue;
        if (elab_read_file(out, src, src_len) == 0) { *out_len = n; return 0; }
    }
    int n = snprintf(out, cap, "%s/%s.tur", dir, name);
    *out_len = n;
    return -1;
}

static bool module_path_exists_named(const char *dir, const char *name,
                                     char *out, size_t cap) {
    for (int k = 0; k < MODULE_FILE_EXT_COUNT; k++) {
        int n = snprintf(out, cap, "%s/%s%s", dir, name, MODULE_FILE_EXTS[k]);
        if (n > 0 && (size_t)n < cap && module_path_exists(out)) return true;
    }
    return false;
}

bool elab_module_resolve_path(Elab *e, const Symbol *name,
                              char *out, size_t cap) {
    const char *base = e->module_base_dir ? e->module_base_dir : ".";
    if (module_path_exists_named(base, name->name, out, cap)) return true;
    if (e->module_stdlib_dir
        && module_path_exists_named(e->module_stdlib_dir, name->name, out, cap)) return true;
    for (int ii = 0; ii < e->n_module_include_dirs; ii++) {
        if (module_path_exists_named(e->module_include_dirs[ii], name->name, out, cap)) return true;
    }
    return false;
}

bool elab_scheme_library_path(void *ud, const char *module, char *out, size_t cap) {
    Elab *e = (Elab *)ud;
    const Symbol *name = symtab_intern(e->st, strslice(module, (uint32_t)strlen(module)));
    return elab_module_resolve_path(e, name, out, cap);
}

/* scheme_lower.h SchemeGlobalFn: what the global `name` of this environment
 * is -- the auto-loaded stdlib's, or an earlier REPL turn's own. */
SchemeGlobalKind elab_scheme_global_kind(void *ud, const char *name) {
    Elab *e = (Elab *)ud;
    StrSlice nm = strslice(name, (uint32_t)strlen(name));
    if (!symtab_contains(e->st, nm)) return SCHEME_GLOBAL_NONE;
    Binding *b = scope_lookup(&e->global, symtab_intern(e->st, nm));
    if (!b) return SCHEME_GLOBAL_NONE;
    if (b->is_from_stdlib || elab_file_is_stdlib(b->span.file_id)) return SCHEME_GLOBAL_STDLIB;
    return elab_prior_turn_global(e, b) ? SCHEME_GLOBAL_EARLIER_TURN : SCHEME_GLOBAL_NONE;
}

/* scheme_lower.h SchemeStdlibFileFn: the auto-loaded stdlib file whose
 * global `name` is -- its basename without `.tur` -- when the session has
 * elaborated the stdlib already (an interpreter or REPL session, a library
 * module imported by a program). */
bool elab_scheme_stdlib_file(void *ud, const char *name, char *out, size_t cap) {
    Elab *e = (Elab *)ud;
    StrSlice nm = strslice(name, (uint32_t)strlen(name));
    if (!symtab_contains(e->st, nm)) return false;
    Binding *b = scope_lookup(&e->global, symtab_intern(e->st, nm));
    if (!b || !(b->is_from_stdlib || elab_file_is_stdlib(b->span.file_id))) return false;
    const SourceFile *sf = diag_source_file(b->span.file_id);
    if (!sf || !sf->path) return false;
    const char *base = sf->path;
    for (const char *p = sf->path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    size_t bl = strlen(base);
    if (bl < 5 || strcmp(base + bl - 4, ".tur") != 0 || bl - 4 >= cap) return false;
    memcpy(out, base, bl - 4);
    out[bl - 4] = '\0';
    return true;
}

/* M2: load one `(import ...)` and inject its :refer symbols (bindings,
 * macros, effects) into the global scope.  The import list of a defmodule
 * header runs through here, and so does a top-level import in a session that
 * allows one (elab_toplevel_import).  false after a diagnostic. */
static bool elab_apply_import(Elab *e, const ImportSpec *imp) {
    if (imp->for_macros) {
        /* Stage 3: macro-time-only import -- evaluate the module into
         * the macro env; no runtime load, no scope injection. */
        char mpath[4096];
        if (!elab_module_resolve_path(e, imp->module_name,
                                      mpath, sizeof(mpath))) {
            diag_emit(DIAG_ERROR, imp->span,
                      ":for-macros module '%s' not found (searched the "
                      "importing file's directory, the stdlib, and the "
                      "-I include dirs)",
                      imp->module_name->name);
            return false;
        }
        if (!elab_macro_env_import(e, imp->module_name, mpath, imp->span)) {
            return false;
        }
        return true;
    }
    ElabModule *loaded = elab_load_module(e, imp->module_name, imp->span);
    if (!loaded) {
        return false;
    }
    /* :refer — add each referred symbol (binding or macro) to the current scope. */
    for (uint32_t k = 0; k < imp->n_refer; k++) {
        const Symbol *ref_sym = imp->refer_syms[k];

        /* First check bindings. */
        Binding *ref_b = NULL;
        for (uint32_t m = 0; m < loaded->n_exports; m++) {
            if (loaded->exports[m]->name == ref_sym) {
                ref_b = loaded->exports[m];
                break;
            }
        }
        if (ref_b) {
            scope_add(&e->global, ref_b);
            continue;
        }

        /* Phase M4: Check exported macros. */
        MacroDef *ref_macro = NULL;
        for (uint32_t m = 0; m < loaded->n_exported_macros; m++) {
            if (loaded->exported_macros[m]->name == ref_sym) {
                ref_macro = loaded->exported_macros[m];
                break;
            }
        }
        if (ref_macro) {
            /* Inject an alias that is visible everywhere via is_referred,
             * but keeps defining_module_name so private helpers of the
             * original module remain accessible during expansion. */
            MacroDef *alias = (MacroDef *)arena_alloc(e->arena, sizeof(MacroDef));
            *alias = *ref_macro;
            alias->is_referred = true;
            /* Check for name collision with existing global macros.
             *
             * The SAME macro arriving by a second path is not a collision.
             * Macro registration is global, so a diamond -- top imports
             * both low and mid, and mid also refers low's macro -- used to
             * report low's macro as conflicting with itself. Re-exporting
             * a macro via (export-from ...) hits the identical shape. A
             * macro is identified by (defining module, name); a module
             * cannot define two macros with one name, so matching both
             * means it is one definition reached twice. Keep the existing
             * registration and move on.
             *
             * A genuine collision -- two DIFFERENT modules exporting the
             * same macro name -- still errors, which is the case the
             * check was written for. */
            MacroDef *prior = elab_lookup_macro(e, ref_sym);
            if (prior != NULL) {
                if (prior->defining_module_name
                    == ref_macro->defining_module_name) {
                    continue;
                }
                diag_emit(DIAG_ERROR, imp->span,
                          "macro '%s' from module '%s' conflicts with an existing macro",
                          ref_sym->name, imp->module_name->name);
                if (prior->defining_module_name)
                    diag_emit(DIAG_NOTE, imp->span,
                              "the macro already in scope is defined by "
                              "module '%s' -- two different modules cannot "
                              "supply the same macro name",
                              prior->defining_module_name->name);
                return false;
            }
            elab_register_macro(e, alias);
            continue;
        }

        diag_emit(DIAG_ERROR, imp->span,
                  "symbol '%s' is not exported from module '%s'",
                  ref_sym->name, imp->module_name->name);
        return false;
    }

    /* PR5-3-D: Process :refer [(effect Name)] imports. */
    for (uint32_t k = 0; k < imp->n_refer_effects; k++) {
        const Symbol *ref_eff_sym = imp->refer_effect_syms[k];
        Effect *ref_eff = NULL;
        for (uint32_t m = 0; m < loaded->n_exported_effects; m++) {
            if (loaded->exported_effects[m]->name == ref_eff_sym) {
                ref_eff = loaded->exported_effects[m];
                break;
            }
        }
        if (!ref_eff) {
            diag_emit(DIAG_ERROR, imp->span,
                      "effect '%s' is not exported from module '%s'",
                      ref_eff_sym->name, imp->module_name->name);
            return false;
        }
        /* Add to referred_effects so the visibility guard allows access. */
        if (e->n_referred_effects >= e->cap_referred_effects) {
            e->cap_referred_effects = e->cap_referred_effects ? e->cap_referred_effects * 2 : 4;
            e->referred_effects = (Effect **)realloc(e->referred_effects,
                                  e->cap_referred_effects * sizeof(Effect *));
        }
        e->referred_effects[e->n_referred_effects++] = ref_eff;
    }
    return true;
}

Expr *elab_toplevel_import(Elab *e, const Form *call) {
    if (!e->toplevel_imports || e->scope != &e->global
        || e->current_module != NULL || e->current_module_name != NULL) {
        diag_emit(DIAG_ERROR, call->span,
                  "import is only allowed inside defmodule");
        return NULL;
    }
    ImportSpec imp;
    if (!parse_import_spec(e, call, &imp)) return NULL;
    if (!elab_apply_import(e, &imp)) return NULL;
    if (imp.alias) {
        if (e->n_toplevel_aliases >= e->cap_toplevel_aliases) {
            uint32_t cap = e->cap_toplevel_aliases ? e->cap_toplevel_aliases * 2 : 4;
            const Symbol **names = (const Symbol **)realloc(
                (void *)e->toplevel_alias_names, cap * sizeof(Symbol *));
            if (!names) { fprintf(stderr, "tur: oom\n"); abort(); }
            e->toplevel_alias_names = names;
            const Symbol **mods = (const Symbol **)realloc(
                (void *)e->toplevel_alias_modules, cap * sizeof(Symbol *));
            if (!mods) { fprintf(stderr, "tur: oom\n"); abort(); }
            e->toplevel_alias_modules = mods;
            e->cap_toplevel_aliases = cap;
        }
        e->toplevel_alias_names[e->n_toplevel_aliases]   = imp.alias;
        e->toplevel_alias_modules[e->n_toplevel_aliases] = imp.module_name;
        e->n_toplevel_aliases++;
    }
    return e_nil(e, call->span);
}

Expr *elab_defmodule(Elab *e, const Form *call) {
    /* Only valid at the top level */
    if (e->scope != &e->global) {
        diag_emit(DIAG_ERROR, call->span, "defmodule is only valid at the top level");
        return NULL;
    }
    /* Syntax: (defmodule name [docstring] (export ...) (import ...)... body...) */
    if (call->as.list.len < 2) {
        diag_emit(DIAG_ERROR, call->span,
                  "defmodule requires a module name: (defmodule name ...)");
        return NULL;
    }
    Form *name_f = call->as.list.items[1];
    if (name_f->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_f->span, "defmodule name must be a symbol");
        return NULL;
    }
    if (!module_name_valid(name_f->as.sym->name, name_f->as.sym->len)) {
        diag_emit(DIAG_ERROR, name_f->span,
                  "invalid module name '%s': only alphanumeric, '-', '_', '/' allowed",
                  name_f->as.sym->name);
        return NULL;
    }
    if (e->has_defmodule) {
        diag_emit(DIAG_ERROR, call->span, "only one defmodule is allowed per file");
        return NULL;
    }
    e->has_defmodule = true;

    uint32_t i = 2;
    const char *docstring = NULL;

    /* Optional docstring */
    if (i < call->as.list.len && call->as.list.items[i]->tag == F_STR) {
        docstring = call->as.list.items[i]->as.s.p;
        i++;
    }

    /* Collect export symbols */
    const Symbol **exports = NULL;
    uint32_t n_exports = 0;
    uint32_t cap_exports = 0;
    const Symbol **exp_effects = NULL;
    uint32_t n_exp_effects = 0;
    uint32_t cap_exp_effects = 0;
    /* G3: names from `(export (mut g))` -- exported AND writable from outside. */
    const Symbol **exp_mut = NULL;
    uint32_t n_exp_mut = 0;
    uint32_t cap_exp_mut = 0;

    /* Collect import specs */
    ImportSpec *imports = NULL;
    uint32_t n_imports = 0;
    uint32_t cap_imports = 0;

    /* (export-from <mod> name ...) -- parallel arrays, one entry per name. */
    const Symbol **reexports = NULL;
    const Symbol **reexport_srcs = NULL;
    uint32_t n_reexports = 0;
    uint32_t cap_reexports = 0;

    /* Consume (export ...) and (import ...) forms */
    while (i < call->as.list.len) {
        Form *item = call->as.list.items[i];
        if (item->tag != F_LIST || item->as.list.len == 0) break;
        Form *head = item->as.list.items[0];
        if (head->tag != F_SYM) break;

        if (head->as.sym == e->sym_export) {
            /* Parse (export sym1 sym2 ... (effect Name) ... (mut g) ...) */
            for (uint32_t j = 1; j < item->as.list.len; j++) {
                Form *sf = item->as.list.items[j];
                if (sf->tag == F_SYM) {
                    /* Regular symbol export */
                    if (n_exports >= cap_exports) {
                        cap_exports = cap_exports ? cap_exports * 2 : 4;
                        exports = (const Symbol **)realloc(exports, cap_exports * sizeof(Symbol *));
                    }
                    exports[n_exports++] = sf->as.sym;
                } else if (sf->tag == F_LIST && sf->as.list.len == 2
                           && sf->as.list.items[0]->tag == F_SYM
                           && sf->as.list.items[0]->as.sym == e->sym_effect) {
                    /* (effect Name) export */
                    Form *ename_f = sf->as.list.items[1];
                    if (ename_f->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, ename_f->span,
                                  "effect name in export list must be a symbol");
                        free(exports); free(exp_effects); free(exp_mut); free(imports);
                    free(reexports); free(reexport_srcs);
                        return NULL;
                    }
                    if (n_exp_effects >= cap_exp_effects) {
                        cap_exp_effects = cap_exp_effects ? cap_exp_effects * 2 : 4;
                        exp_effects = (const Symbol **)realloc(exp_effects, cap_exp_effects * sizeof(Symbol *));
                    }
                    exp_effects[n_exp_effects++] = ename_f->as.sym;
                } else if (sf->tag == F_LIST && sf->as.list.len == 2
                           && sf->as.list.items[0]->tag == F_SYM
                           && sf->as.list.items[0]->as.sym == e->sym_export_mut) {
                    /* G3: (mut g) -- export g AND permit writes from outside.
                     * The name is exported normally as well, so a reader needs
                     * no second entry. */
                    Form *gname_f = sf->as.list.items[1];
                    if (gname_f->tag != F_SYM) {
                        diag_emit(DIAG_ERROR, gname_f->span,
                                  "name in (mut ...) export must be a symbol");
                        free(exports); free(exp_effects); free(exp_mut); free(imports);
                    free(reexports); free(reexport_srcs);
                        return NULL;
                    }
                    if (n_exports >= cap_exports) {
                        cap_exports = cap_exports ? cap_exports * 2 : 4;
                        exports = (const Symbol **)realloc(exports, cap_exports * sizeof(Symbol *));
                    }
                    exports[n_exports++] = gname_f->as.sym;
                    if (n_exp_mut >= cap_exp_mut) {
                        cap_exp_mut = cap_exp_mut ? cap_exp_mut * 2 : 4;
                        exp_mut = (const Symbol **)realloc(exp_mut, cap_exp_mut * sizeof(Symbol *));
                    }
                    exp_mut[n_exp_mut++] = gname_f->as.sym;
                } else {
                    diag_emit(DIAG_ERROR, sf->span,
                              "export list entries must be symbols, (effect Name), "
                              "or (mut global) forms");
                    free(exports); free(exp_effects); free(exp_mut); free(imports);
                    free(reexports); free(reexport_srcs);
                    return NULL;
                }
            }
            i++;
        } else if (head->as.sym == e->sym_export_from) {
            /* Parse (export-from <mod> name1 name2 ...).  Only shape is
             * checked here; that <mod> is imported and actually exports each
             * name is validated after the body is elaborated, alongside the
             * ordinary export list. */
            if (item->as.list.len < 3) {
                diag_emit(DIAG_ERROR, item->span,
                          "(export-from ...) needs a module name and at least "
                          "one symbol -- (export-from other-module foo bar)");
                free(exports); free(exp_effects); free(exp_mut); free(imports);
                free(reexports); free(reexport_srcs);
                return NULL;
            }
            Form *src_f = item->as.list.items[1];
            if (src_f->tag != F_SYM) {
                diag_emit(DIAG_ERROR, src_f->span,
                          "the module in (export-from ...) must be a symbol");
                free(exports); free(exp_effects); free(exp_mut); free(imports);
                free(reexports); free(reexport_srcs);
                return NULL;
            }
            for (uint32_t j = 2; j < item->as.list.len; j++) {
                Form *nf = item->as.list.items[j];
                if (nf->tag != F_SYM) {
                    diag_emit(DIAG_ERROR, nf->span,
                              "(export-from %s ...) entries must be symbols",
                              src_f->as.sym->name);
                    free(exports); free(exp_effects); free(exp_mut); free(imports);
                    free(reexports); free(reexport_srcs);
                    return NULL;
                }
                if (n_reexports >= cap_reexports) {
                    cap_reexports = cap_reexports ? cap_reexports * 2 : 4;
                    reexports = (const Symbol **)realloc(
                        reexports, cap_reexports * sizeof(Symbol *));
                    reexport_srcs = (const Symbol **)realloc(
                        reexport_srcs, cap_reexports * sizeof(Symbol *));
                }
                reexports[n_reexports]       = nf->as.sym;
                reexport_srcs[n_reexports++] = src_f->as.sym;
            }
            i++;
        } else if (head->as.sym == e->sym_import) {
            /* Parse (import module-name [:as alias] [:refer [syms...]]) */
            if (n_imports >= cap_imports) {
                cap_imports = cap_imports ? cap_imports * 2 : 4;
                imports = (ImportSpec *)realloc(imports, cap_imports * sizeof(ImportSpec));
            }
            if (!parse_import_spec(e, item, &imports[n_imports])) {
                free(exports); free(exp_effects); free(exp_mut); free(imports);
                    free(reexports); free(reexport_srcs);
                return NULL;
            }
            n_imports++;
            i++;
        } else {
            break; /* Body starts here */
        }
    }

    uint32_t body_start = i;

    /* M1: Allocate module struct early and populate imports so elab_lookup_sym
     * can resolve qualified names and import aliases during body elaboration. */
    DefModule *mod = (DefModule *)arena_alloc(e->arena, sizeof(DefModule));
    memset(mod, 0, sizeof(DefModule));
    mod->name = name_f->as.sym;
    mod->docstring = docstring;

    /* Copy exports to arena (used for marking after pass 2) */
    if (n_exports > 0) {
        mod->exports = (const Symbol **)arena_alloc(e->arena, n_exports * sizeof(Symbol *));
        for (uint32_t j = 0; j < n_exports; j++) mod->exports[j] = exports[j];
        mod->n_exports = n_exports;
    }
    free(exports); exports = NULL;

    /* G3: copy the writable-export list to the arena beside the exports. */
    if (n_exp_mut > 0) {
        mod->exports_mut = (const Symbol **)arena_alloc(e->arena, n_exp_mut * sizeof(Symbol *));
        for (uint32_t j = 0; j < n_exp_mut; j++) mod->exports_mut[j] = exp_mut[j];
        mod->n_exports_mut = n_exp_mut;
    }
    free(exp_mut); exp_mut = NULL;

    /* PR5-3-B: Copy exported_effects to arena */
    if (n_exp_effects > 0) {
        mod->exported_effects = (const Symbol **)arena_alloc(e->arena, n_exp_effects * sizeof(Symbol *));
        for (uint32_t j = 0; j < n_exp_effects; j++) mod->exported_effects[j] = exp_effects[j];
        mod->n_exported_effects = n_exp_effects;
    }
    free(exp_effects); exp_effects = NULL;

    /* Copy the re-export list to the arena beside the exports. */
    if (n_reexports > 0) {
        mod->reexports = (const Symbol **)arena_alloc(
            e->arena, n_reexports * sizeof(Symbol *));
        mod->reexport_srcs = (const Symbol **)arena_alloc(
            e->arena, n_reexports * sizeof(Symbol *));
        for (uint32_t j = 0; j < n_reexports; j++) {
            mod->reexports[j]     = reexports[j];
            mod->reexport_srcs[j] = reexport_srcs[j];
        }
        mod->n_reexports = n_reexports;
    }
    free(reexports); reexports = NULL;
    free(reexport_srcs); reexport_srcs = NULL;

    /* Copy imports to arena (needed for alias resolution during elaboration) */
    if (n_imports > 0) {
        mod->imports = (ImportSpec *)arena_alloc(e->arena, n_imports * sizeof(ImportSpec));
        for (uint32_t j = 0; j < n_imports; j++) mod->imports[j] = imports[j];
        mod->n_imports = n_imports;
    }
    free(imports); imports = NULL;

    /* M1: Set module context — body bindings will inherit defining_module_name */
    e->current_module_name = mod->name;
    e->current_module = mod;

    /* M2: Process imports — load each referenced module and inject :refer symbols. */
    for (uint32_t j = 0; j < mod->n_imports; j++) {
        if (!elab_apply_import(e, &mod->imports[j])) {
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }
    }

    /* Pass 1: forward-declare all defn bodies (for mutual recursion) */
    elab_forward_declare_defns(e, call->as.list.items, body_start,
                               call->as.list.len);

    /* Pass 2: elaborate body forms.
     *
     * On a per-form failure (elab_form returns NULL) record the error but KEEP
     * GOING, so every bad form in the module surfaces its own diagnostic.  This
     * mirrors the top-level driver in elaborate_program (elab_toplevel.c), which
     * sets rc=-1 and continues rather than bailing on the first NULL.  The old
     * behaviour returned NULL on the FIRST failing form, so a (defmodule ...)
     * with N independent type errors reported only the first one -- e.g. the
     * tourist swap_reject_test negative fixture (every defn is a deliberate
     * swap-rejection probe) emitted only one of its ~20 expected TUR-E0001s, so
     * a regression at any later probe would slip through unnoticed.  After the
     * loop a module that had any failure still returns NULL so the caller knows
     * elaboration failed; only successfully-elaborated forms are kept in body[]. */
    uint32_t n_body = call->as.list.len - body_start;
    Expr **body = (n_body == 0) ? NULL :
        (Expr **)arena_alloc(e->arena, n_body * sizeof(Expr *));
    uint32_t actual_n_body = 0;
    bool body_had_error = false;

    /* typeclass-method-resolution-ignores-the-class (symptom A), defmodule
     * half.  A spice is defmodule-wrapped, so this loop -- not
     * elaborate_program's Pass 2 -- is where a spice's `definstance` below a
     * use is decided.  Same treatment, same reasoning: elaborate such a defn
     * speculatively and, on failure, retry it once every body form has been
     * processed.  See the comment on Pass 2 in elab_toplevel.c.
     *
     * `slot[]` is indexed by ORIGINAL body position (not fill order) so a
     * deferred form keeps its place; body[] is compacted from it afterwards,
     * leaving emission order exactly as written. */
    uint32_t n_slots = call->as.list.len - body_start;
    Expr **slot = (n_slots == 0) ? NULL :
        (Expr **)arena_alloc(e->arena, n_slots * sizeof(Expr *));
    bool *md_deferred = (n_slots == 0) ? NULL :
        (bool *)calloc(n_slots, sizeof(bool));
    for (uint32_t k = 0; k < n_slots; k++) slot[k] = NULL;
    bool md_any_deferred = false;
    /* forward-call-to-generic-callee-typed-as-placeholder: a defn that calls a
     * generic defn of this body not elaborated yet waits for it, as at top
     * level (fwd_gen_order_init in elab_toplevel.c). */
    FwdGenOrder fgo;
    fwd_gen_order_init(&fgo, e, call->as.list.items + body_start, n_slots);
    Form *const *md_forms = call->as.list.items + body_start;

    const Form *saved_tl_stmt = e->toplevel_stmt;
    MdRetryCtx md_ctx = { e, mod, md_forms, n_slots, saved_tl_stmt, slot,
                          md_deferred, &md_any_deferred, &body_had_error,
                          &fgo };
    for (uint32_t s = 0; s < n_slots; s++) {
        if (fwd_gen_order_should_defer(&fgo, md_forms, s)) {
            fwd_gen_order_defer(&fgo, s);
            md_any_deferred = true;
            continue;
        }
        /* A waiting defn this form names is elaborated first, so the form
         * sees its definition, as it did when the defn kept its place. */
        md_flush(&md_ctx, md_forms[s], s);
        md_elab_slot(&md_ctx, s, s);
    }

    /* Second chance for the deferred defns; every instance is registered now.
     * No capture frame -- a still-failing body reports for real.  A defn that
     * waited for a generic callee comes after it (fwd_gen_order_drain). */
    if (md_any_deferred)
        fwd_gen_order_drain(&fgo, md_forms, md_deferred, md_retry_slot,
                            md_probe_slot, &md_ctx);
    fwd_gen_order_free(&fgo);
    free(md_deferred);
    md_deferred = NULL;

    /* Compact in ORIGINAL order. */
    for (uint32_t k = 0; k < n_slots; k++)
        if (slot[k]) body[actual_n_body++] = slot[k];

    if (body_had_error) {
        e->current_module_name = NULL;
        e->current_module = NULL;
        return NULL;
    }

    mod->body = body;
    mod->n_body = actual_n_body;

    /* M1: Mark exported bindings and validate they exist in this module.
     * Phase M4: macro names in the export list are also valid exports. */
    for (uint32_t j = 0; j < mod->n_exports; j++) {
        Binding *exp_b = scope_lookup(&e->global, mod->exports[j]);
        if (!exp_b || exp_b->defining_module_name != mod->name) {
            /* Not a binding — check if it's a macro defined in this module. */
            bool found_macro = false;
            for (uint32_t k = 0; k < e->n_macros; k++) {
                if (e->macros[k]->name == mod->exports[j] &&
                    e->macros[k]->defining_module_name == mod->name) {
                    found_macro = true;
                    break;
                }
            }
            if (!found_macro) {
                diag_emit(DIAG_ERROR, call->span,
                          "exported symbol '%s' is not defined in this module",
                          mod->exports[j]->name);
                diag_emit(DIAG_NOTE, call->span,
                          "ensure '%s' has a matching (defn ...) or (defmacro ...) inside the (defmodule %s ...) body, "
                          "and check for typos in the (export ...) list",
                          mod->exports[j]->name, mod->name->name);
                e->current_module_name = NULL;
                e->current_module = NULL;
                return NULL;
            }
            /* Macro exports don't need is_exported flag; they're collected by elab_load_module. */
        } else {
            exp_b->is_exported = true;
            /* G3: a global listed as `(mut g)` is writable from outside.
             * Rejected by name on anything that cannot be written, rather than
             * accepted and left inert -- `(export (mut some-defn))` would
             * otherwise be a silently-meaningless annotation, which is the
             * exact shape the `def` annotation audit exists to prevent. */
            for (uint32_t k = 0; k < mod->n_exports_mut; k++) {
                if (mod->exports_mut[k] != mod->exports[j]) continue;
                /* A top-level `defn` is `is_global` too, so staticness alone
                 * does not separate a data global from a function.  The fn type
                 * (or a backing FnDef) is what does. */
                if (!exp_b->is_global || exp_b->type.kind == TY_FN ||
                    exp_b->source_fn_def != NULL) {
                    diag_emit(DIAG_ERROR, call->span,
                              "(export (mut %s)): '%s' is not a mutable global -- "
                              "`(mut ...)` marks an exported global as writable "
                              "from outside this module; export a function plainly",
                              mod->exports[j]->name, mod->exports[j]->name);
                    e->current_module_name = NULL;
                    e->current_module = NULL;
                    return NULL;
                }
                if (!exp_b->is_mut) {
                    diag_emit(DIAG_ERROR, call->span,
                              "(export (mut %s)): '%s' is an immutable global -- "
                              "nothing can write it; declare it `^mut` or export "
                              "it plainly",
                              mod->exports[j]->name, mod->exports[j]->name);
                    e->current_module_name = NULL;
                    e->current_module = NULL;
                    return NULL;
                }
                exp_b->is_export_mut = true;
                break;
            }
        }
    }

    /* Validate (export-from <mod> name ...).  Runs after the body so the
     * imports have been processed and the source module's bindings are in the
     * global scope.  Nothing is marked or emitted here: the binding is already
     * `is_exported` in its own module, and a consumer of THIS module resolves
     * to that same Binding -- same mangled symbol, no forwarding wrapper.
     * What this loop does is prove each claim, so a typo or a missing import
     * is an error here rather than an unresolved symbol in a downstream file. */
    for (uint32_t j = 0; j < mod->n_reexports; j++) {
        const Symbol *src  = mod->reexport_srcs[j];
        const Symbol *nm   = mod->reexports[j];

        /* The source module must be imported.  export-from deliberately does
         * not load on its own: a second load path with its own resolution
         * rules is how two modules end up disagreeing about which file a name
         * came from. */
        bool imported = false;
        for (uint32_t k = 0; k < mod->n_imports; k++) {
            if (mod->imports[k].module_name == src) { imported = true; break; }
        }
        if (!imported) {
            diag_emit(DIAG_ERROR, call->span,
                      "(export-from %s %s): module '%s' is not imported by "
                      "this module",
                      src->name, nm->name, src->name);
            diag_emit(DIAG_NOTE, call->span,
                      "add (import %s) to (defmodule %s ...) -- export-from "
                      "re-exports from a module you already import, it does "
                      "not load one",
                      src->name, mod->name->name);
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }

        /* The question is "does `src` EXPORT this name", not "does `src`
         * DEFINE it" -- otherwise a chain (low -> mid -> hi) breaks at the
         * second hop, where mid re-exports a name low defined.  So consult
         * src's own export table, which already carries its re-exports. */
        ElabModule *srcmod = elab_find_loaded_module(e, src);
        bool exported_there = false;
        if (srcmod) {
            for (uint32_t k = 0; k < srcmod->n_exports; k++) {
                if (srcmod->exports[k]->name == nm) { exported_there = true; break; }
            }
            for (uint32_t k = 0; !exported_there && k < srcmod->n_exported_macros; k++) {
                if (srcmod->exported_macros[k]->name == nm) { exported_there = true; break; }
            }
        }
        if (exported_there) continue;

        /* Not exported by src.  Separate "defined there but private" from
         * "not there at all" -- the first is a visibility decision the author
         * of src made, the second is a typo, and telling them apart is the
         * difference between one edit and a hunt. */
        Binding *rb = scope_lookup(&e->global, nm);
        bool defined_there = (rb && rb->defining_module_name == src);
        if (!defined_there) {
            for (uint32_t k = 0; k < e->n_macros; k++) {
                if (e->macros[k]->name == nm &&
                    e->macros[k]->defining_module_name == src) {
                    defined_there = true;
                    break;
                }
            }
        }
        if (defined_there) {
            diag_emit(DIAG_ERROR, call->span,
                      "(export-from %s %s): '%s' is defined by '%s' but not "
                      "exported from it",
                      src->name, nm->name, nm->name, src->name);
        } else {
            diag_emit(DIAG_ERROR, call->span,
                      "(export-from %s %s): '%s' is not exported by module "
                      "'%s'",
                      src->name, nm->name, nm->name, src->name);
        }
        e->current_module_name = NULL;
        e->current_module = NULL;
        return NULL;
    }

    /* PR5-3-B: Validate and mark exported effects. */
    for (uint32_t j = 0; j < mod->n_exported_effects; j++) {
        const Symbol *eff_name = mod->exported_effects[j];
        Effect *eff = effect_env_lookup(e->effect_env, eff_name);
        if (!eff || eff->defining_module_name != mod->name) {
            diag_emit(DIAG_ERROR, call->span,
                      "exported effect '%s' is not defined in this module",
                      eff_name->name);
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }
        if (eff->is_private) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0021_PRIVATE_EFFECT,
                                "private effect '%s' cannot be exported",
                                eff_name->name);
            e->current_module_name = NULL;
            e->current_module = NULL;
            return NULL;
        }
        eff->is_exported = true; /* explicit export — already true, but record intent */
    }

    /* Reset module context */
    e->current_module_name = NULL;
    e->current_module = NULL;

    Expr *out = expr_new(e->arena, EX_DEFMODULE, TYPE_NIL, call->span);
    out->as.defmodule_.mod = mod;
    return out;
}

/* used-attr-whole-program: force a module to be loaded (and thus emitted)
 * even though no `(import)` reaches it.  This retains a `#[used]` defn that is
 * reached only through its raw mangled C symbol -- a hand-written cross-module
 * inline-C bridge or by-address C-ABI callback -- on the single-file /
 * whole-program build path (`tur build <file>`, `tur run <file>`, `tur test`),
 * which inlines only the entry's Turmeric import closure and would otherwise
 * drop the module.  Loading is idempotent (elab_load_module dedups by name),
 * so force-loading an already-imported module is a no-op; and because no
 * `:refer` is applied, none of the module's names enter the entry's scope --
 * its module-private defns are simply registered for file-scope emission, just
 * as an ordinary `(import ...)` of the module would do.  No-op under separate
 * compilation (every project module is compiled and linked there already). */
void elab_force_load_module(Elab *e, const char *module_name) {
    if (!e || !module_name || !*module_name) return;
    if (e->separate_compilation) return;
    const Symbol *sym =
        symtab_intern(e->st, strslice(module_name, (uint32_t)strlen(module_name)));
    Span span = {0, 0, 0, 0, 0, 0};
    (void)elab_load_module(e, sym, span);
}

/* M1: Resolve a symbol with module visibility and qualified name support.
 * Returns the binding on success.
 * Returns NULL with *had_error = false: symbol not found, caller emits "unbound".
 * Returns NULL with *had_error = true: error already emitted, caller returns NULL. */
Binding *elab_lookup_sym(Elab *e, const Symbol *sym, Span span, bool *had_error) {
    *had_error = false;

    /* Direct scope lookup */
    Binding *b = scope_lookup(e->scope, sym);
    if (b) {
        /* The `tur/` namespace is implicitly imported everywhere, so a binding
         * defined in a `tur/` module is globally visible regardless of its
         * (export ...) list.  The M7 promotion sweep (elab_toplevel.c) normally
         * establishes that by rewriting every `tur/`-module binding's
         * defining_module_name to NULL -- but that sweep fires exactly once, at
         * the stdlib/user boundary.  On the interpreter's incremental
         * re-elaboration (TR2, now the default) the accumulated stdlib prefix
         * grows across evals, so the boundary -- and thus the promotion -- can
         * land after a form that already needed the binding.  The visible
         * symptom is a macro-expanded call to a module-private stdlib helper:
         * `(assert! ...)` expands to `tur-contract-check`, which is private to
         * tur/contract, and every contract/sized/existential fixture failed
         * with "symbol 'tur-contract-check' is private to module 'tur/contract'"
         * under --interpret while passing on the whole-program path.
         *
         * Honour the implicit `tur/` import directly at lookup time so
         * visibility no longer depends on the sweep having already run.  This
         * is the binding-side counterpart of the identical rule in
         * elab_lookup_macro (elab_core.c), added for the same reason. */
        if (b->defining_module_name != NULL
            && b->defining_module_name->len >= 4
            && memcmp(b->defining_module_name->name, "tur/", 4) == 0) {
            /* visible: implicitly-imported stdlib namespace */
        } else
        /* Visibility: private symbol accessed from outside its module */
        if (b->defining_module_name != NULL
            && e->current_module_name != b->defining_module_name
            && !b->is_exported) {
            diag_emit(DIAG_ERROR, span,
                      "symbol '%s' is private to module '%s'",
                      sym->name, b->defining_module_name->name);
            diag_emit(DIAG_NOTE, b->span,
                      "defined here; add '%s' to module '%s''s (export ...) list to expose it",
                      sym->name, b->defining_module_name->name);
            *had_error = true;
            return NULL;
        }
        /* F4 (cross-plan-followups): emit deprecation warning at the use
         * site.  Suppressed for self-recursive references so a deprecated
         * function does not warn on its own internal recursion.  Under
         * --Werror=deprecated the diagnostic is promoted to an error so
         * the elaborator stops compilation. */
        if (b->is_deprecated && b->name != e->current_fn_name) {
            DiagLevel sev = g_werror_deprecated ? DIAG_ERROR : DIAG_WARNING;
            if (b->deprecation_message) {
                diag_emit(sev, span,
                          "'%s' is deprecated: %s",
                          sym->name, b->deprecation_message);
            } else {
                diag_emit(sev, span,
                          "'%s' is deprecated", sym->name);
            }
            if (g_werror_deprecated) {
                *had_error = true;
                return NULL;
            }
        }
        return b;
    }

    /* Qualified name resolution — only if symbol contains '/' */
    const char *sym_str = sym->name;
    uint32_t    sym_len = sym->len;
    bool has_slash = false;
    for (uint32_t i = 0; i < sym_len; i++) {
        if (sym_str[i] == '/') { has_slash = true; break; }
    }
    if (!has_slash) return NULL;

    /* Self-qualified: current module name is a prefix */
    if (e->current_module_name != NULL) {
        const Symbol *mn = e->current_module_name;
        if (sym_len > mn->len + 1
            && sym_str[mn->len] == '/'
            && memcmp(sym_str, mn->name, mn->len) == 0) {
            const char *suffix     = sym_str + mn->len + 1;
            uint32_t    suffix_len = sym_len - mn->len - 1;
            const Symbol *ss = symtab_intern(e->st, strslice(suffix, suffix_len));
            Binding *b2 = scope_lookup(e->scope, ss);
            if (b2) return b2;
            diag_emit_with_code(DIAG_ERROR, span, TUR_E0003_UNBOUND_SYMBOL,
                                "unbound symbol '%.*s' in module '%s'",
                                (int)suffix_len, suffix, mn->name);
            *had_error = true;
            return NULL;
        }
    }

    /* M2: Cross-module qualified resolution. */
    if (e->current_module != NULL) {
        /* alias/sym — match by :as alias */
        for (uint32_t i = 0; i < e->current_module->n_imports; i++) {
            const ImportSpec *imp = &e->current_module->imports[i];
            if (!imp->alias) continue;
            const Symbol *alias = imp->alias;
            if (sym_len > alias->len + 1
                && sym_str[alias->len] == '/'
                && memcmp(sym_str, alias->name, alias->len) == 0) {
                const char *sym_part     = sym_str + alias->len + 1;
                uint32_t    sym_part_len = sym_len - alias->len - 1;
                const Symbol *sym_key = symtab_intern(e->st, strslice(sym_part, sym_part_len));
                ElabModule *loaded = elab_find_loaded_module(e, imp->module_name);
                if (!loaded) {
                    diag_emit(DIAG_ERROR, span,
                              "module '%s' (alias '%s') was not loaded",
                              imp->module_name->name, alias->name);
                    *had_error = true;
                    return NULL;
                }
                for (uint32_t m = 0; m < loaded->n_exports; m++) {
                    if (loaded->exports[m]->name == sym_key)
                        return loaded->exports[m];
                }
                /* M7: check if the symbol IS defined in that module but private. */
                Binding *priv = NULL;
                for (uint32_t k = 0; k < e->global.n; k++) {
                    Binding *gb = e->global.bindings[k];
                    if (gb->name == sym_key &&
                        gb->defining_module_name == imp->module_name) {
                        priv = gb; break;
                    }
                }
                diag_emit(DIAG_ERROR, span,
                          "symbol '%s' is not exported from module '%s'",
                          sym_key->name, imp->module_name->name);
                if (priv) {
                    diag_emit(DIAG_NOTE, priv->span,
                              "'%s' is defined here but is private; add it to module '%s''s (export ...) list",
                              sym_key->name, imp->module_name->name);
                }
                *had_error = true;
                return NULL;
            }
        }
        /* full-module-name/sym — match by full module name */
        for (uint32_t i = 0; i < e->current_module->n_imports; i++) {
            const ImportSpec *imp = &e->current_module->imports[i];
            const Symbol *mn = imp->module_name;
            if (sym_len > mn->len + 1
                && sym_str[mn->len] == '/'
                && memcmp(sym_str, mn->name, mn->len) == 0) {
                const char *sym_part     = sym_str + mn->len + 1;
                uint32_t    sym_part_len = sym_len - mn->len - 1;
                const Symbol *sym_key = symtab_intern(e->st, strslice(sym_part, sym_part_len));
                ElabModule *loaded = elab_find_loaded_module(e, mn);
                if (!loaded) {
                    diag_emit(DIAG_ERROR, span,
                              "module '%s' was not loaded", mn->name);
                    *had_error = true;
                    return NULL;
                }
                for (uint32_t m = 0; m < loaded->n_exports; m++) {
                    if (loaded->exports[m]->name == sym_key)
                        return loaded->exports[m];
                }
                /* M7: check if the symbol IS defined in that module but private. */
                Binding *priv = NULL;
                for (uint32_t k = 0; k < e->global.n; k++) {
                    Binding *gb = e->global.bindings[k];
                    if (gb->name == sym_key && gb->defining_module_name == mn) {
                        priv = gb; break;
                    }
                }
                diag_emit(DIAG_ERROR, span,
                          "symbol '%s' is not exported from module '%s'",
                          sym_key->name, mn->name);
                if (priv) {
                    diag_emit(DIAG_NOTE, priv->span,
                              "'%s' is defined here but is private; add it to module '%s''s (export ...) list",
                              sym_key->name, mn->name);
                }
                *had_error = true;
                return NULL;
            }
        }
    }

    /* qualified-module-calls-unresolved-at-toplevel: `(Foo/bar)` OUTSIDE any
     * defmodule.  Neither path above runs there (no current module, so no
     * self-qualification and no imports to search), so the name fell to the
     * TUR-W0040 runtime dispatch -- an error on the compiled path, and under
     * the interpreter a lookup of the mangled `Foo_slbar` that misses an
     * EXPORTED member (`Foo__bar`) while finding a private one by its bare
     * name: exactly inverted.  The top level is the program's own scope and
     * sees every module defined or loaded into it, the way it sees `tur/`.
     * A module's members are globals tagged with their defining module, so
     * split at each `/` from the right (module names nest: `a/b/f` tries
     * module `a/b` first) and look for that member of that module. */
    if (e->current_module == NULL && e->current_module_name == NULL) {
        /* A session's top-level `(import m :as a)`: `a/name` is m's export.
         * The latest import of an alias wins. */
        for (uint32_t i = e->n_toplevel_aliases; i-- > 0; ) {
            const Symbol *alias = e->toplevel_alias_names[i];
            if (sym_len <= alias->len + 1 || sym_str[alias->len] != '/'
                || memcmp(sym_str, alias->name, alias->len) != 0)
                continue;
            const Symbol *mn = e->toplevel_alias_modules[i];
            const Symbol *sym_key = symtab_intern(
                e->st, strslice(sym_str + alias->len + 1, sym_len - alias->len - 1));
            ElabModule *loaded = elab_find_loaded_module(e, mn);
            if (loaded) {
                for (uint32_t m = 0; m < loaded->n_exports; m++)
                    if (loaded->exports[m]->name == sym_key)
                        return loaded->exports[m];
            }
            diag_emit(DIAG_ERROR, span,
                      "symbol '%s' is not exported from module '%s'",
                      sym_key->name, mn->name);
            *had_error = true;
            return NULL;
        }
        for (uint32_t cut = sym_len; cut-- > 1; ) {
            if (sym_str[cut] != '/' || cut + 1 >= sym_len) continue;
            const Symbol *mn = symtab_intern(e->st, strslice(sym_str, cut));
            const Symbol *sym_key =
                symtab_intern(e->st, strslice(sym_str + cut + 1, sym_len - cut - 1));
            Binding *member = NULL;
            for (uint32_t k = 0; k < e->global.n; k++) {
                Binding *gb = e->global.bindings[k];
                if (gb->name == sym_key && gb->defining_module_name == mn) {
                    member = gb; break;
                }
            }
            if (!member) continue;
            if (member->is_exported) return member;
            diag_emit(DIAG_ERROR, span,
                      "symbol '%s' is not exported from module '%s'",
                      sym_key->name, mn->name);
            diag_emit(DIAG_NOTE, member->span,
                      "'%s' is defined here but is private; add it to module '%s''s (export ...) list",
                      sym_key->name, mn->name);
            *had_error = true;
            return NULL;
        }
    }

    return NULL; /* Not a recognised qualified name; caller handles "unbound" */
}
