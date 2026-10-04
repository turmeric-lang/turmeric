/* elab_call.c -- function-call elaboration, partial application, polymorphic dispatch. */
/* _GNU_SOURCE for the glibc extensions this TU's includes reach for; must
 * precede every include so the system headers see it wherever they are first
 * pulled in. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "elab_internal.h"
#include "lang_dialects.h"   /* saffron-lang-plan S3: lang_span_is_dynamic */
#include "scheme_lower.h"   /* r7rs: scheme_span_is_user_source, scheme_public_name */
#include "cps.h"          /* cps_expr_uses_control -- the control-widen hoist */
bool sum_box_reader_name(const char *nm);  /* emit_core.c; see emit_internal.h */
bool expr_is_fresh_any_closure(const Expr *x);  /* emit_core.c; see emit_internal.h */
#include "experiments.h"  /* Slice 3 (constrained-hkt-forall): hkt-hrt gate */
#include "mono_specs.h"   /* VBM1 (van-laarhoven-monomorphization): spec registry */

/* macro-depth-guard-loses-race-with-asan-stack: the real-stack-headroom
 * probe that is the second trigger of the macro-expansion depth guard
 * below.  Shared with the emitter's expression-depth guard, which had the
 * identical race -- see stack_guard.h. */
#include "stack_guard.h"

/* --- Slice 3 (constrained-hkt-forall-plan): higher-kinded rank-2 helpers ----
 *
 * A rank-2 `forall` parameter may quantify a higher-kinded variable
 * (`(f :: * -> *)`, kind established by slice 1) and use it applied as `(f a)`
 * in its body.  At each instantiation site -- both where the poly fn is passed
 * (its container parameter's concrete type) and where it is invoked inside the
 * callee (the actual argument's type) -- the type filling `f` must be a type
 * application whose base constructor's kind matches `f`'s declared kind.
 *
 * Turmeric's HRT is type-erased, so a carrier-compatible container (a
 * parametric opaque/heap constructor) flows through the int64 `tur_poly_fn_t`
 * carrier unchanged; a by-value *aggregate* product container does not fit the
 * carrier and would emit broken C, so it is rejected here as not-yet-supported
 * rather than miscompiled (deferred; see
 * docs/archive/history/hrt-hkt-aggregate-container-carrier.md). */

/* Does this forall quantify a higher-kinded (arrow-kind) bound variable?
 * KIND_STAR (plain type var) and KIND_ROW/KIND_TYPEROW (effect/type rows) do
 * not count. */

/* hkt-generic-forwarded-bind-continuation-segfaults: does the callee's
 * declared type for argument slot `idx` return an HKT-erased `(M b)` -- a
 * tyvar-headed application?  Every consumer of a function passed there calls
 * it through the erased carrier cast and reads its result as a carrier. */
static bool sink_fn_result_is_hkt_erased(const Type *fn_type, uint32_t idx) {
    if (!fn_type || fn_type->kind != TY_FN || !fn_type->as.fn.arg_full_types ||
        idx >= fn_type->as.fn.arity)
        return false;
    const Type *decl = fn_type->as.fn.arg_full_types[idx];
    const Type *dr = (decl && decl->kind == TY_FN) ? decl->as.fn.result_full_type : NULL;
    const Type *hh = dr;
    while (hh && hh->kind == TY_APP) hh = hh->as.app.fn;
    return dr && dr->kind == TY_APP && hh && hh->kind == TY_TYVAR;
}

/* fnsan-nil-closure-into-erased-result: does the callee's declared type for
 * argument slot `idx` return a bare type variable (`(fn [...] A)`)?  At
 * `A := nil` no spec clone is made, so the carrier base calls the slot as
 * returning a word. */
static bool sink_fn_result_is_bare_tyvar(const Type *fn_type, uint32_t idx) {
    if (!fn_type || fn_type->kind != TY_FN || !fn_type->as.fn.arg_full_types ||
        idx >= fn_type->as.fn.arity)
        return false;
    const Type *decl = fn_type->as.fn.arg_full_types[idx];
    if (!decl || decl->kind != TY_FN) return false;
    const Type *dr = decl->as.fn.result_full_type;
    return dr ? dr->kind == TY_TYVAR : decl->as.fn.result_kind == TY_TYVAR;
}

/* fnsan-fat-closure-at-tyvar-sink: does the callee's declared type for
 * argument slot `idx` take a bare type variable in some PARAMETER position
 * (`(fn [A A] bool)`)?  A callee that reads the slot erased calls it with
 * words there. */
static bool sink_fn_has_tyvar_param(const Type *fn_type, uint32_t idx) {
    if (!fn_type || fn_type->kind != TY_FN || !fn_type->as.fn.arg_full_types ||
        idx >= fn_type->as.fn.arity)
        return false;
    const Type *decl = fn_type->as.fn.arg_full_types[idx];
    if (!decl || decl->kind != TY_FN) return false;
    for (uint32_t k = 0; k < decl->as.fn.arity; k++) {
        const Type *pt = decl->as.fn.arg_full_types ? decl->as.fn.arg_full_types[k] : NULL;
        if (pt ? pt->kind == TY_TYVAR
               : (decl->as.fn.arg_kinds && decl->as.fn.arg_kinds[k] == TY_TYVAR))
            return true;
    }
    return false;
}

/* ...and is `a` a `^fat` parameter declared with a fn type, being forwarded?
 * Its reference reads as the opaque `ptr<void>` handle; the binding keeps the
 * declared `(fn [W W] bool)`, which a spec clone resolves to its own types --
 * `map-eq?` forwarding its comparator to the inline-C `map-eq-raw?`.  Only
 * an inline-C callee is asked about it: a Turmeric-bodied one reads the
 * handle through its own spec or carrier base, which the producer that boxed
 * it already answered for. */
static bool arg_is_typed_fat_param(const Expr *a) {
    return a && a->kind == EX_VAR && a->as.var.binding &&
           a->as.var.binding->is_fat && a->as.var.binding->is_param &&
           a->as.var.binding->type.kind == TY_FN;
}

/* ...and is `t` a by-value aggregate (a concrete ADT monomorph or ADT, not a
 * :heap one) -- the result shape the boxing shim bridges? */
static bool type_is_byvalue_aggregate_result(Type t) {
    if (t.kind == TY_APP)
        return type_app_is_concrete_adt(&t) && !type_is_heap_adt(t) &&
               !type_is_heap_struct(t);
    if (t.kind == TY_ADT)
        return t.as.adt_.def != NULL && !type_is_heap_adt(t) && !type_is_heap_struct(t);
    return false;
}

static bool forall_has_higher_kinded_var(const Type *forall_ty) {
    if (!forall_ty || forall_ty->kind != TY_FORALL) return false;
    if (!forall_ty->as.forall_.var_kinds) return false;
    for (uint8_t i = 0; i < forall_ty->as.forall_.n_vars; i++) {
        Kind k = forall_ty->as.forall_.var_kinds[i];
        if (k != KIND_STAR && k != KIND_ROW && k != KIND_TYPEROW) return true;
    }
    return false;
}

/* hkt-hrt GRADUATED 2026-07-06: a rank-2 forall over a higher-kinded variable is
 * always accepted; the former --enable=hkt-hrt gate is gone.  The instantiation
 * sites still validate the actual container kind (hrt_validate_hk_actual below).
 */

/* If `body_param` is `(f a)` where `f` is one of the forall's higher-kinded
 * bound variables, return f's declared kind via *out_kind and true; else false.
 * Only single-application `(f a)` formals participate (the shape the lens/optic
 * use-cases and the slice-3 fixtures exercise). */
static bool hrt_body_param_hk_var_kind(const Type *forall_ty,
                                       const Type *body_param, Kind *out_kind) {
    if (!body_param || body_param->kind != TY_APP || !body_param->as.app.fn)
        return false;
    const Type *head = body_param->as.app.fn;
    if (head->kind != TY_TYVAR || !head->as.tyvar_.name) return false;
    for (uint8_t i = 0; i < forall_ty->as.forall_.n_vars; i++) {
        const char *vn = forall_ty->as.forall_.var_names[i];
        Kind vk = forall_ty->as.forall_.var_kinds
                    ? forall_ty->as.forall_.var_kinds[i] : KIND_STAR;
        if (vn && strcmp(vn, head->as.tyvar_.name) == 0 &&
            vk != KIND_STAR && vk != KIND_ROW && vk != KIND_TYPEROW) {
            if (out_kind) *out_kind = vk;
            return true;
        }
    }
    return false;
}

/* Validate that `actual` is a type application whose base constructor kind
 * matches `f_kind`, and that its container is carrier-compatible.  `ctx_what`
 * names the site for the diagnostic ("rank-2 argument" / "rank-2 call").
 * Returns false after emitting TUR-E0306/E0307 on violation. */
static bool hrt_validate_hk_actual(Elab *e, Kind f_kind, Type actual,
                                   Span span, const char *ctx_what) {
    (void)e;
    if (actual.kind != TY_APP) {
        diag_emit(DIAG_ERROR, span,
                  "%s: expected a type application instantiating the "
                  "higher-kinded variable (e.g. (Box int)), but got a "
                  "non-application type (TUR-E0306)", ctx_what);
        return false;
    }
    /* Walk the application spine to the base constructor and compare its
     * unapplied kind against f's declared kind. */
    const Type *base = &actual;
    while (base->kind == TY_APP && base->as.app.fn) base = base->as.app.fn;
    if (base->hkt_kind != f_kind) {
        diag_emit(DIAG_ERROR, span,
                  "%s: container constructor of kind '%s' cannot instantiate a "
                  "higher-kinded variable of kind '%s' (TUR-E0307)",
                  ctx_what, kind_to_string(base->hkt_kind),
                  kind_to_string(f_kind));
        return false;
    }
    /* A by-value aggregate product container now flows through the erased int64
     * poly carrier via the heap-box bridge (emit_agg_box/unbox + the poly-wrapper
     * poly_agg_arg_mask and the carrier-spill shim), so it is accepted -- no
     * longer the deferred TUR-E0297 case. */
    return true;
}

/* ---- file-local helper forward declarations ---- */
static Expr *elab_call_hamt_fn(Elab *e, Span span, const Symbol *fn_name, uint32_t n_args, Expr **args);
static Expr *elab_lower_map_call(Elab *e, const Form *call, const Symbol *name);
static Expr *elab_partial_apply(Elab *e, const Form *call, Binding *fn_binding,
    Type fn_type, Expr **elab_args, uint32_t n_provided);
static Expr *elab_call_fn(Elab *e, const Form *call, Binding *fn_binding);
static Expr *elab_poly_call(Elab *e, const Form *call, Binding *fn_binding);
static Expr *elab_call_head_expr(Elab *e, const Form *call, Expr *head_expr);
static void mark_direct_apply_niche_word_params(Elab *e, Expr *head_expr,
                                               Expr *call_expr);

/* GS5/CS3: shared AbiTypeBinding lives in expr.h so emit can consume what
 * elaboration produced. Keep CallTypeBinding as a local alias for minimal
 * churn in the body of this file. */
typedef AbiTypeBinding CallTypeBinding;

/* Discoverability aid (one-off-script-print-and-annotation-ergonomics, Finding
 * 2): the named scalar print/convert helpers are *not* auto-loaded. A user
 * probing "how do I print/convert a float" reaches for these by analogy and
 * hits a bare "unknown function or operator", with no pointer to where they
 * live or how to pull them in. Map the well-known helpers to their stdlib file
 * so the diagnostic can suggest the exact `(load ...)` line.
 *
 * NB: these files (stdlib/math.tur, stdlib/bits.tur) are bare definition files,
 * not `defmodule`s with `(export ...)`, so `(import math :refer [float->int])`
 * does *not* work -- the only mechanism is `(load "stdlib/<file>")`. The hint
 * deliberately suggests `load`, not a broken import. Curated on purpose: every
 * entry names a genuine helper (verified by the load-hint fixtures), so the
 * hint never points at a nonexistent symbol or file. Helpers a user might
 * *expect* but that do not exist (println-int, float->cstr) are deliberately
 * absent -- those stay a plain "unknown". */
const char *tur_stdlib_load_hint(const char *name) {
    static const struct { const char *name; const char *file; } table[] = {
        { "float->int",   "stdlib/math.tur" },
        { "int->float",   "stdlib/math.tur" },
        { "printf-float6", "stdlib/math.tur" },
        { "println-float", "stdlib/bits.tur" },
        /* Classic Lisp list surface: thin aliases over list-head/list-tail/
         * tnil?/list-length.  list.tur is auto-loaded normally, so this hint
         * only surfaces under a :no-stdlib build -- where (load ...) is the fix. */
        { "car",    "stdlib/list.tur" },
        { "cdr",    "stdlib/list.tur" },
        { "null?",  "stdlib/list.tur" },
        { "length", "stdlib/list.tur" },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0) return table[i].file;
    }
    return NULL;
}

static const char *stdlib_load_hint_file(const Symbol *name) {
    return tur_stdlib_load_hint(name->name);
}

/* list-length-on-cons-any-segfaults: the carrier-level list helpers in
 * stdlib/list.tur walk a cons chain as `struct { int64_t head; int64_t tail;
 * }`.  A `(Cons A)` coerces to their `:int` parameter in argument position,
 * and for most A that is sound -- a scalar or pointer head is one word, so the
 * tail sits at offset 8.  Two element kinds widen the head and push the tail
 * past offset 8, and the walk then reads the tail out of the middle of the
 * head:
 *
 *   - `any` / a union: the head is a two-word tur_tagged_t.  This is every
 *     list in a Saffron program -- `(list 1 2 3)`, a `& xs : any` rest list
 *     -- so `(length xs)` segfaulted and `(list-head xs)` answered the tag.
 *   - a by-value aggregate laid out inline (`(Cons (Option int))`).
 *
 * For those, a call to one of the helpers below is elaborated as a call to
 * its element-aware twin instead, which reads `.head` / `.tail` at the
 * monomorph's own layout.  The twins take the same list and answer the same
 * question, typed: `thead` returns the element (an `any`, a `(Option int)`)
 * rather than its first word.  Every other element keeps the carrier call
 * and its emitted C byte-for-byte. */
static const char *carrier_list_typed_twin(const char *name) {
    static const struct { const char *carrier; const char *typed; } table[] = {
        { "list-length", "tlength" }, { "length", "tlength" },
        { "list-head",   "thead"   }, { "car",    "thead"   },
        { "list-tail",   "ttail"   }, { "cdr",    "ttail"   },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].carrier) == 0) return table[i].typed;
    }
    return NULL;
}

/* Is `t` a stdlib `(Cons A)` whose head is wider than the carrier word? */
static bool cons_head_shifts_carrier_tail(const Type *t) {
    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(t, &def, args, &n_args) || !def || n_args != 1 ||
            !def->name || strcmp(def->name, "Cons") != 0 || !def->is_heap)
        return false;
    const Type *elem = &args[0];
    if (elem->kind == TY_CONTRACT && elem->as.contract_.base_type)
        elem = elem->as.contract_.base_type;
    if (elem->kind == TY_ANY || elem->kind == TY_UNION) return true;
    return repr_of(elem, REPR_POS_STRUCT_FIELD) == REPR_BYVAL_AGG;
}

/* Called with the elaborated carrier call; returns the twin call, or NULL to
 * keep `call_expr`.  The argument is found under the implicit coercions the
 * call wrapped it in, but NOT under an ascription the source wrote: an
 * explicit `(:: xs :int)` is the documented escape hatch that discards the
 * element type, and stays exactly as unsafe as it says it is. */
static Expr *typed_list_twin_redirect(Elab *e, const Form *call,
                                      const Binding *fn_binding,
                                      const Expr *call_expr) {
    if (!call_expr || call_expr->kind != EX_CALL || call->as.list.len != 2)
        return NULL;
    if (!fn_binding || !fn_binding->is_global || !fn_binding->name ||
            !fn_binding->name->name)
        return NULL;
    if (!fn_binding->is_from_stdlib &&
            !elab_file_is_stdlib(fn_binding->span.file_id))
        return NULL;
    const char *twin = carrier_list_typed_twin(fn_binding->name->name);
    if (!twin || call_expr->as.call_.n_args != 1) return NULL;
    const Expr *arg = call_expr->as.call_.args[0];
    while (arg && !cons_head_shifts_carrier_tail(&arg->type)) {
        if (arg->kind == EX_ASCRIBE && !arg->as.ascribe_.type_form)
            arg = arg->as.ascribe_.inner;
        else if (arg->kind == EX_CAST)
            arg = arg->as.cast_.expr;
        else if (arg->kind == EX_REINTERPRET)
            arg = arg->as.reinterpret_.expr;
        else
            return NULL;
    }
    if (!arg) return NULL;
    const Symbol *twin_sym = intern_cstr(e->st, twin);
    bool qual_err = false;
    Binding *twin_b = elab_lookup_sym(e, twin_sym, call->span, &qual_err);
    if (!twin_b || !twin_b->is_global || twin_b->type.kind != TY_FN) return NULL;
    /* The argument form elaborates a second time here.  Only the wide-head
     * case pays that, and it is the case that used to crash. */
    Form **items = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
    items[0] = form_sym(e->arena, call->as.list.items[0]->span, twin_sym);
    items[1] = call->as.list.items[1];
    return elab_call(e, form_list(e->arena, call->span, items, 2));
}

/* docs/archive/history/defn-shadows-return-special-form.md: head-position dispatch in
 * elab_call (below) matches special forms by symbol identity *before* any
 * binding, macro, or typeclass-method lookup.  A user `(defn return ...)` is
 * therefore accepted, bound, and then never consulted: every bare
 * `(return ...)` elaborates as the early-return form, and the resulting
 * diagnostic lands on the CALLER's argument type with no mention of the
 * shadowing.  `return` is a natural name for a monadic unit, so this is easy
 * to hit and very hard to read.
 *
 * This table drives the TUR-W0042 warning emitted at the DEFINITION site, which
 * is where the mistake actually is.  Membership rule -- a name belongs here
 * only if elab_call dispatches it UNCONDITIONALLY:
 *
 *   - Names gated on `!scope_lookup(e->scope, name)` are deliberately
 *     shadowable (`handler`, `with`, `default-of`, and the session ops
 *     `send`/`recv`/`close`/...).  A user defn of those wins, so they are NOT
 *     reserved and must stay out of this table.
 *   - Names dispatched only at one arity or one argument shape (`async`,
 *     `await`, `select`, `atomically`, `check`, `or-else`, the `tvar-*` ops,
 *     `thread-spawn`) are omitted too: a defn at another arity is genuinely
 *     callable, and this check runs before the parameter list is parsed.
 *   - The map surface (`assoc`, `get`, `count`, `merge`, ...) routes through
 *     elab_lower_map_call, which falls back to ordinary call resolution for a
 *     non-persistent receiver.  Not reserved.
 *
 * Inside a `defmodule`, a shadowing definition is still reachable through its
 * QUALIFIED name (`mod/return`) -- the qualified head symbol is a different
 * symbol and never matches a special form -- which is why the warning speaks
 * about the bare name specifically. */
static const char *const reserved_special_forms_[] = {
    /* core binding / control forms */
    "def", "define", "let", "let*", "letrec", "if", "do", "unsafe", "set!",
    "while", "case", "defer", "return", "match", "quote", "gensym",
    "^tailcall",
    /* definition forms */
    "defn", "fn", "\xce\xbb", "extern-c", "defmacro", "defmodule", "import",
    "export", "load", "defstruct", "make-struct", "defopaque", "defdata",
    "defgadt", "defclass", "definstance", "defkind", "defrec", "deftype",
    "defalias", "defdynamic", "defeffect", "defprotocol",
    /* generators */
    "gen", "yield", "gen-next", "gen-done?",
    /* references, rc, weak */
    "ref", "deref", "drop!", "lref/new", "rc/of", "rc/clone", "rc/drop",
    "rc->ptr", "rc/strong-count", "rc/from-ref", "ref/from-rc", "weak",
    "upgrade", "weak?", "ref?",
    /* delimited continuations */
    "reset", "shift", "shift0", "call/cc", "call/cc*", "escape",
    "cloneable-reset", "cloneable-shift", "serial-reset", "serial-shift",
    "cont?",
    /* algebraic effects */
    "binding", "perform", "handle", "handle-shallow", "try-with",
    "with-handler", "resume", "discontinue", "compose-handlers",
    /* types, casts, ascription */
    "as", "type-of", "cast", "is?", "coerce", "&", "&mut",
    "forall", "exists", "type-app", "::", "pack", "open",
    /* sessions -- the definition/constructor forms only; the value-level ops
     * are shadowable and deliberately absent */
    "make-protocol", "make-session",
    /* panic / unwinding */
    "panic", "panic-with", "catch-unwind", "catch-panic-of",
    "panic-payload-type", "panic-payload-value", "panic-payload-file",
    "panic-payload-line", "panic-payload-downcast",
    /* unsafe primitives */
    "ptr-deref", "ptr-write", "ptr-add", "ptr-sub", "ptr-null?", "ptr-of",
    "unsafe-cast", "reinterpret", "transmute", "array-get-unchecked",
    "array-set-unchecked", "raw-malloc", "raw-free", "raw-realloc",
    "raw-memcpy", "raw-memset", "c-call", "call-ptr", "dlopen", "dlsym",
    "dlclose",
    /* STM */
    "stm", "retry",
    /* GC */
    "gc!", "gc-enable!", "gc-disable!", "gc-auto!", "gc-collections",
    "gc-objects-freed", "gc-live-blocks", "gc-candidate-high-water",
    /* misc operators */
    "?", "->", "->>",
};

bool tur_name_is_reserved_special_form(const char *name) {
    if (!name) return false;
    /* `(.method obj ...)` is dispatched on the leading dot, so any name that
     * starts with `.` is equally unreachable as a bare call head. */
    if (name[0] == '.' && name[1] != '\0') return true;
    for (size_t i = 0;
         i < sizeof(reserved_special_forms_) / sizeof(reserved_special_forms_[0]);
         i++) {
        if (strcmp(name, reserved_special_forms_[i]) == 0) return true;
    }
    return false;
}

/* Emit TUR-W0042 when `name` (a defn/defmacro name being defined at `span`)
 * collides with a reserved special form.  Callers suppress it for stdlib
 * auto-load and for re-elaborated specialization clones. */
void tur_warn_if_shadows_special_form(const Symbol *name, Span span,
                                      const char *form_kind) {
    if (!name || !tur_name_is_reserved_special_form(name->name)) return;
    diag_emit_with_code(DIAG_WARNING, span, TUR_W0042_SHADOWS_SPECIAL_FORM,
        "%s '%s' shadows the special form '%s'; a bare (%s ...) call always "
        "elaborates as the special form, so this definition is unreachable by "
        "its bare name -- rename it",
        form_kind, name->name, name->name, name->name);
}

/* Migration aid for legacy C-backed spice code.  A handful of forms that older
 * "store a pointer as :int and hand-roll allocation + field access" code reaches
 * for were never Turmeric language operators -- they only ever existed inside an
 * inline-C block, or not at all -- so they surface as a bare "unknown function or
 * operator" with no path forward.  Map each to a one-line "use X instead" so the
 * upgrade is discoverable.  Curated and fixed-name (no false positives); the
 * `Struct-field` accessor case is handled separately because it needs the struct
 * registry to know whether `<name>` really decomposes as `<struct>-<field>`. */
const char *tur_legacy_form_hint(const char *name) {
    if (strcmp(name, "sizeof") == 0)
        return "sizeof is only valid inside an inline-C block; for a heap buffer "
               "use a stdlib (Vec T) via vec-new/vec-push!, or call malloc from "
               "an (extern-c malloc [size : int] : ptr<void>) block";
    if (strcmp(name, "float64*") == 0 || strcmp(name, "float32*") == 0)
        return "raw-pointer indexing (float64*/float32*) is not a Turmeric form; "
               "store the data in a stdlib (Vec float) and use (vec-get v i) / "
               "(vec-set! v i x)";
    if (strcmp(name, "declare") == 0)
        return "declare is not a Turmeric form; declare an external C symbol with "
               "(extern-c name [arg : T ...] : ret)";
    return NULL;
}

/* True when `name` decomposes as `<struct>-<field>` for some registered struct
 * and one of its fields -- i.e. an attempt to call a generated accessor function
 * (`(mat-rows m)`) that Turmeric does not emit.  On a hit, write a `(.field x)`
 * read suggestion into `buf` (the field name is used in the suggested form so the
 * pointer is concrete).  Struct names may themselves contain hyphens, so we test
 * each registered struct as a prefix rather than splitting on the first '-'. */
static bool struct_accessor_hint(Elab *e, const char *name,
                                 char *buf, size_t buflen) {
    if (!e || !name) return false;
    size_t nlen = strlen(name);
    /* A `defstruct` lowers to a single-variant record ADT, so the
     * `<name>-<field>` accessor-call mistake lands in adt_defs; scan there so
     * the hint fires (record fields back `(.field x)` exactly like a struct).
     * structdef-retirement DS-C: the parallel scan over the (now always-empty)
     * struct_defs registry is dead and removed. */
    for (uint32_t i = 0; i < e->n_adt_defs; i++) {
        const struct AdtDef *ad = e->adt_defs[i];
        if (!ad || !ad->name || ad->n_ctors != 1) continue;
        const struct CtorDef *ct = ad->ctors[0];
        if (!ct || !ct->is_record) continue;
        size_t slen = strlen(ad->name);
        if (slen + 1 >= nlen) continue;
        if (strncmp(name, ad->name, slen) != 0) continue;
        if (name[slen] != '-') continue;
        const char *field = name + slen + 1;
        for (uint32_t f = 0; f < ct->n_fields; f++) {
            if (ct->fields[f].name && strcmp(field, ct->fields[f].name) == 0) {
                snprintf(buf, buflen,
                         "struct accessor functions are not generated; read the "
                         "field with (.%s x) and construct with (make-struct %s "
                         "...)", field, ad->name);
                return true;
            }
        }
    }
    return false;
}

/* TY2.2: Coerce a value expression to the `any` top type by wrapping it in an
 * EX_UNION_INJECT carrying the value's TypeKind as the runtime tag.  Used at
 * every widening site (call args, return position, branch unification) so a
 * narrower value flowing into an `any` slot is boxed exactly once.
 *
 * Carrier-compatible payloads (int/bool/float/nil/cstr/ptr and ADT handles)
 * ride the int64_t carrier directly.  By-value structs cannot, so box_struct
 * is set and codegen emits a heap copy.  Already-`any` values pass through
 * unchanged (no double-boxing).  Returns NULL only on allocation paths that
 * cannot happen (defensive). */
/* cps-coloring-walk-has-no-arm-for-union-inject: bind `value` to a fresh temp
 * and hand back an `EX_VAR` reading it, filling `lb` with the let binding.
 *
 * The one primitive behind both hoists below.  A CPS-lowerable node can hold a
 * control operator in a LET INIT but not as an operand of a node the IR only
 * ever DELEGATES, so moving it to an init is the whole trick.  The report
 * proposed doing this in the CPS IR and found it blocked -- it would need an
 * `Expr` referring to a CPS-bound `CVar`, which nothing can synthesise. Here
 * the temp is an ordinary binding and no such bridge is needed. */
Expr *elab_bind_control_temp(Elab *e, Expr *value, LetBinding *lb) {
    char nm[48];
    snprintf(nm, sizeof nm, "__ctlhoist_%u", elab_fresh_id(e));
    const Symbol *sym = symtab_intern(e->st, strslice(nm, (uint32_t)strlen(nm)));
    Binding *tb = binding_new(e, sym, value->type, false, false, value->span);
    memset(lb, 0, sizeof(*lb));
    lb->binding = tb;
    lb->init = value;
    Expr *v = expr_new(e->arena, EX_VAR, value->type, value->span);
    v->as.var.binding = tb;
    return v;
}

/* cps-coloring-walk-has-no-arm-for-union-inject: hoist every control-bearing
 * operand of a Saffron dynamic node into an enclosing `let`, so what remains at
 * the node is a variable.
 *
 * The CPS IR admits these nodes only as DELEGATABLE ones -- an ordinary
 * direct-emitted call, once nothing under them threads the DK -- exactly like
 * the `any` widen beside them.  With a `call/cc` still in an operand they are
 * `BODY-UNSUPPORTED` and the control op reaches the direct emitter, which
 * aborts.  `(+ x (call/cc ...))` on an `any` `x` and `(f (call/cc ...))` on an
 * `any` `f` are the two shapes the report reproduced.
 *
 * Bindings are emitted in operand order, which a single `let` evaluates in
 * order, so evaluation order is unchanged.  A node with no control-bearing
 * operand is returned untouched -- this costs nothing for ordinary Saffron code,
 * which is almost all of it. */
/* saffron-effect-row-lost-through-unannotated-call: does this operand hold a
 * call to a named function, without crossing a function boundary?
 *
 * The companion to `cps_expr_uses_control` in the hoist below, and the whole
 * point is that the two are different questions.  That one asks whether a
 * lexical control OPERATOR is in the subtree; this one whether a CALL is.  A
 * call to an effectful function is not a control operator, so the control probe
 * alone left `(+ (g) 1)` un-hoisted -- and an un-hoisted operand is what lets
 * the CPS IR hand the whole dynamic node to the direct emitter, which calls
 * `g`'s direct entry, opens a fresh DK root, and never sees the caller's
 * handler.  Hoisting puts the call at a bind position, where it lowers to the
 * DK-threaded cps->cps edge instead.
 *
 * Why a SYNTACTIC probe rather than "is the callee effectful": at elaboration
 * the callee's effect row does not exist yet -- `effect_check` is a later pass
 * -- and any ordering-based answer would be wrong under forward references and
 * mutual recursion.  So the probe is coarse and the GATE is what keeps it
 * narrow: the caller only applies it in a unit that declares an effect at all
 * (`unit_has_user_effect`), which is where an un-threaded call can do harm.
 *
 * A nested fn/closure body is its own call graph and is not descended. */
static bool saffron_operand_has_call(const Expr *op) {
    if (!op) return false;
    /* A NIL-typed operand is never hoisted: the hoist binds the value to a
     * local, and `void __ctlhoist_N = f(...)` is "declared void" / "void value
     * not ignored".  `(when (> n 0) (do (println n) (countdown ...)))` in the
     * tour guide's own fixture is the shape -- a `do` whose value is unit, which
     * has a call in it and nothing to bind.  Nothing is lost by declining: a
     * unit-valued operand delivers no word to the enclosing node, so there is no
     * seam for it to cross. */
    if (op->type.kind == TY_NIL) return false;
    switch (op->kind) {
        case EX_CALL:
            /* effect-row-lost-through-a-constructor-argument: a CONSTRUCTOR call
             * is NOT a call for hoisting purposes.  It stores its
             * (independently-elaborated) argument values into a fresh aggregate
             * and invokes nothing, so it can never reach a `perform` -- the same
             * reasoning as the constructor leaf exemption in `cps_collect_calls`
             * (docs/archive/history/cps-coloring-overcolors-nonnode-calls.md).
             * Its own arguments are ordinary expressions and are still walked
             * below, so `(Box (Inner (g)))` hoists on `(g)` as it should.
             *
             * Counting one as a call made the hoist wrap a pure constructor in a
             * `let`, and a ctor call's Expr carries data its caller reads back
             * off the EX_CALL node -- `size_index`, stamped by
             * `sz8_infer_ctor_size_index` just above the hoist site.  Wrapped,
             * the sized-GADT checks found no index and TUR-E0260 silently
             * stopped firing (errors/sized-cross-param-reject and
             * errors/sized-return-claim-gadt-reject, on the INTERPRETED path
             * only -- `tur check` still reported it, which is why run.sh stayed
             * green and only run-turi.sh caught it). */
            if (op->as.call_.ctor) {
                for (uint32_t i = 0; i < op->as.call_.n_args; i++)
                    if (saffron_operand_has_call(op->as.call_.args[i])) return true;
                return false;
            }
            return true;
        case EX_REINTERPRET:      return saffron_operand_has_call(op->as.reinterpret_.expr);
        case EX_ASCRIBE:      return saffron_operand_has_call(op->as.ascribe_.inner);
        case EX_UNION_INJECT: return saffron_operand_has_call(op->as.union_inject_.value);
        case EX_ANY_CAST:     return saffron_operand_has_call(op->as.any_cast_.value);
        case EX_ANY_IS:       return saffron_operand_has_call(op->as.any_is_.value);
        case EX_ANY_TYPE_OF:  return saffron_operand_has_call(op->as.any_type_of_.value);
        case EX_GET_FIELD:    return saffron_operand_has_call(op->as.get_field_.struct_expr);
        /* The composite forms a widened BODY is usually made of.  Without these
         * the return-position hoist saw `(do (g) 1)` as call-free and the whole
         * body -- colored call included -- stayed one delegatable operand. */
        case EX_DO:
            for (uint32_t i = 0; i < op->as.do_.n; i++)
                if (saffron_operand_has_call(op->as.do_.items[i])) return true;
            return false;
        case EX_LET:
            for (uint32_t i = 0; i < op->as.let_.n; i++)
                if (saffron_operand_has_call(op->as.let_.bindings[i].init)) return true;
            return saffron_operand_has_call(op->as.let_.body);
        case EX_IF:
            return saffron_operand_has_call(op->as.if_.cond)
                || saffron_operand_has_call(op->as.if_.then_)
                || saffron_operand_has_call(op->as.if_.else_or_null);
        case EX_DYN_OP:
            for (uint32_t i = 0; i < op->as.dyn_op_.n_args; i++)
                if (saffron_operand_has_call(op->as.dyn_op_.args[i])) return true;
            return false;
        case EX_DYN_CALL:
            if (saffron_operand_has_call(op->as.dyn_call_.fn)) return true;
            for (uint32_t i = 0; i < op->as.dyn_call_.n_args; i++)
                if (saffron_operand_has_call(op->as.dyn_call_.args[i])) return true;
            return false;
        case EX_DYN_FIELD:    return saffron_operand_has_call(op->as.dyn_field_.obj);
        case EX_DYN_METHOD:
            if (saffron_operand_has_call(op->as.dyn_method_.obj)) return true;
            for (uint32_t i = 0; i < op->as.dyn_method_.n_args; i++)
                if (saffron_operand_has_call(op->as.dyn_method_.args[i])) return true;
            return false;
        default:
            return false;
    }
}

Expr *elab_hoist_control_operands(Elab *e, Expr *node) {
    if (!node) return node;
    Expr **slots[32];
    uint32_t n_slots = 0;
    switch (node->kind) {
        case EX_DYN_OP:
            for (uint32_t i = 0; i < node->as.dyn_op_.n_args && n_slots < 32; i++)
                slots[n_slots++] = &node->as.dyn_op_.args[i];
            break;
        case EX_DYN_CALL:
            slots[n_slots++] = &node->as.dyn_call_.fn;
            for (uint32_t i = 0; i < node->as.dyn_call_.n_args && n_slots < 32; i++)
                slots[n_slots++] = &node->as.dyn_call_.args[i];
            break;
        case EX_DYN_FIELD:
            slots[n_slots++] = &node->as.dyn_field_.obj;
            break;
        case EX_DYN_METHOD:
            slots[n_slots++] = &node->as.dyn_method_.obj;
            for (uint32_t i = 0; i < node->as.dyn_method_.n_args && n_slots < 32; i++)
                slots[n_slots++] = &node->as.dyn_method_.args[i];
            break;
        /* effect-row-lost-through-a-constructor-argument: the two TYPED nodes
         * that reach the CPS translation as leaves, for the same reason the
         * dynamic nodes above do -- the operand is emitted in direct style, so
         * a colored call inside it opens a fresh DK root and never sees the
         * caller's handler.
         *
         * A CONSTRUCTOR call only.  An ordinary EX_CALL resolves to a binding
         * and the CPS translation ANF-binds its arguments itself; a ctor call
         * has no `fn_binding`, so it takes the indirect-callee arm and is
         * rejected outright ("indirect call (non-atomic args)").  Hoisting the
         * argument is what turns `(Box (g))` into the `(let [n (g)] (Box n))`
         * the report names as the shape a fix has to preserve -- literally, as
         * it happens.
         *
         * EX_GET_FIELD is hoisted for the same reason and is needed on the same
         * repro: with the ctor hoisted, `(.v <let>)` still holds the colored
         * call one level down, and a `let` operand is no more delegatable than
         * the call was.  Bound out, the field read is over a plain local. */
        case EX_CALL:
            /* ...and a dict-dispatched method call, the same indirect-callee
             * arm for the same reason (method-call-control-operand-evicted). */
            if (!node->as.call_.ctor && !node->as.call_.dict_arg) return node;
            for (uint32_t i = 0; i < node->as.call_.n_args && n_slots < 32; i++)
                slots[n_slots++] = &node->as.call_.args[i];
            break;
        case EX_GET_FIELD:
            slots[n_slots++] = &node->as.get_field_.struct_expr;
            break;
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < node->as.make_struct_.n_fields && n_slots < 32; i++)
                slots[n_slots++] = &node->as.make_struct_.field_values[i];
            break;
        default:
            return node;
    }
    /* saffron-effect-row-lost-through-unannotated-call: a CALL in an operand is
     * hoisted too, in a unit that declares an effect.  See
     * saffron_operand_has_call for why the probe is syntactic and why the gate
     * is what keeps it narrow.
     *
     * NOT for `and` / `or`, which are LAZY.  Hoisting binds an operand in a
     * `let` that runs BEFORE the node, which is exactly what the short-circuit
     * must not do -- `(and (f false) (f (boom)))` has to not reach `boom`, and
     * with the operand hoisted it did (saffron-dyn-truthy caught it, which is
     * what that fixture's short-circuit row is there for).  The CONTROL half of
     * the hoist has always had the same hazard and is left exactly as it was:
     * this is the new half declining, not a change to the old one. */
    bool lazy_op = node->kind == EX_DYN_OP && node->as.dyn_op_.op &&
                   (strcmp(node->as.dyn_op_.op->name, "and") == 0 ||
                    strcmp(node->as.dyn_op_.op->name, "or")  == 0);
    #define SAFFRON_HOIST_OPERAND(OP)                                          \
        ((OP) && (cps_expr_uses_control(OP)                                    \
                  || (!lazy_op && e->unit_has_user_effect                       \
                      && saffron_operand_has_call(OP))))
    uint32_t n_hoist = 0;
    for (uint32_t i = 0; i < n_slots; i++)
        if (SAFFRON_HOIST_OPERAND(*slots[i])) n_hoist++;
    if (n_hoist == 0) return node;

    LetBinding *lbs = (LetBinding *)arena_alloc(e->arena, n_hoist * sizeof(LetBinding));
    uint32_t h = 0;
    for (uint32_t i = 0; i < n_slots; i++) {
        if (!SAFFRON_HOIST_OPERAND(*slots[i])) continue;
        *slots[i] = elab_bind_control_temp(e, *slots[i], &lbs[h]);
        h++;
    }
    Expr *let = expr_new(e->arena, EX_LET, node->type, node->span);
    let->as.let_.bindings = lbs;
    let->as.let_.n = n_hoist;
    let->as.let_.body = node;
    #undef SAFFRON_HOIST_OPERAND
    return let;
}

/* saffron-dynamic-surface-pass H8: a TYPED function value entering an `any`
 * in a Saffron file gets an all-`any` adaptor.
 *
 * A dynamic call checks the box's signature id against the all-`any`
 * signature of the call site, so `(app inc-typed 41)` -- `inc-typed` being
 * `(fn [int] int)` -- panicked "cannot call this function here" while the
 * interpreter answered 42.  Rather than teach the dynamic call every
 * signature, wrap the value once, where it is boxed, in
 * `(fn [__da0 ...] (NAME __da0 ...))`: the lambda's parameters take the
 * dialect's `any` default, the call inside it goes through the ordinary
 * checked seam per argument, and its return takes the `any` pin -- all
 * machinery that already exists.  Only a NAMED value (a defn or a local
 * bound to a function) can be re-referenced by a form, so the adaptor
 * covers those; a typed closure produced by an expression stays as it was. */
/* `type_to_form` spells a primitive as a KEYWORD (`:int`), which is the
 * defstruct field spelling; a `fn` parameter list and a `cast` target want the
 * bare symbol.  Normalise, and pass everything else (named ADTs, TY_APP
 * chains) through untouched. */
static Form *saffron_seam_type_form(Elab *e, const Type *t, Span sp) {
    /* `type_to_form` has no spelling for `any` (its other callers decline on
     * it), and the `any`-bridge adaptor needs one for every `any` slot. */
    if (t && t->kind == TY_ANY)
        return form_sym(e->arena, sp, symtab_intern(e->st, strslice("any", 3)));
    Form *f = type_to_form(e, t, sp);
    if (f && f->tag == F_KEYWORD) return form_sym(e->arena, sp, f->as.sym);
    return f;
}

/* saffron-dynamic-surface-pass H7: the INBOUND twin of
 * `saffron_dyn_fn_adaptor` -- the seam from an `any` that holds a function
 * INTO a typed function parameter.
 *
 * The outbound adaptor (H8) wraps a typed function so an `any` can hold it:
 * every parameter and the result become `any`, and the box records the
 * all-`any` id.  So by the time a function reaches an `any`, that is what it
 * IS -- the fat box's slot 0 is a
 * `tur_tagged_t (*)(void *, tur_tagged_t)` shim, whatever the original
 * signature was.
 *
 * That is why the two obvious repairs are both wrong, and both were tried:
 *   - Passing the payload word straight through gives a `(void *)` to a
 *     three-word `tur_poly_fn_t` parameter -- `incompatible type for
 *     argument 1`, a whole TU cc rejects.
 *   - Making the box's word into a `tur_poly_fn_t` and relaxing the cast's
 *     id check COMPILES, and is a silent wrong answer: the callee then calls
 *     an all-`any` shim with a raw `int64_t` where a 16-byte `tur_tagged_t`
 *     is expected.  The filed fix direction proposed exactly this, limited to
 *     captureless lambdas.
 *
 * A representation this different needs MARSHALLING, not a reinterpret, so
 * synthesise the wrapper that does it:
 *
 *   (let [__sfn <the any-valued argument>]
 *     (fn [__sa0 : A0 ...] : R (cast (__sfn __sa0 ...) R)))
 *
 * `(__sfn __sa0 ...)` is a dynamic call on an `any` head, which boxes each
 * argument and yields an `any`; the `cast` unboxes the result to `R` with the
 * ordinary checked-cast panic if the function returned something else.  The
 * `let` matters: without it the argument expression would be re-evaluated on
 * every call of the adaptor.
 *
 * Going through the BOX rather than a static wrapper is what makes this work
 * for a CAPTURING closure too -- `(mk 10)` -- which the reinterpret could
 * never have handled.
 *
 * Returns NULL to decline (an all-`any` target, which needs no marshalling and
 * gets the plain checked unbox; a signature too wide; or a parameter/result
 * type `type_to_form` cannot spell as source), leaving the caller's existing
 * path untouched. */
/* True when every parameter and the result of `t` are `any` -- the signature
 * H8's outbound adaptor gives every boxed function, and the one the two
 * adaptors decline to marshal (nothing to marshal). */
static bool fn_type_is_all_any(const Type *t) {
    if (!t || t->kind != TY_FN) return false;
    if (t->as.fn.result_kind != TY_ANY) return false;
    for (uint32_t k = 0; k < t->as.fn.arity; k++)
        if (t->as.fn.arg_kinds[k] != TY_ANY) return false;
    return true;
}

static Expr *saffron_seam_fn_adaptor(Elab *e, const Form *arg_form,
                                     const Type *want, Span sp) {
    if (!e || !arg_form || !want || want->kind != TY_FN) return NULL;
    if (want->as.fn.cfnptr) return NULL;
    uint32_t n = want->as.fn.arity;
    if (n > 5) return NULL;
    /* An all-`any` target is already the representation the box holds, so the
     * plain checked unbox is right and a wrapper would only add a call. */
    bool all_any = (want->as.fn.result_kind == TY_ANY);
    for (uint32_t k = 0; k < n && all_any; k++)
        if (want->as.fn.arg_kinds[k] != TY_ANY) all_any = false;
    if (all_any) return NULL;

    const Symbol *fn_s   = symtab_intern(e->st, strslice("fn", 2));
    const Symbol *let_s  = symtab_intern(e->st, strslice("let", 3));
    const Symbol *cast_s = symtab_intern(e->st, strslice("cast", 4));
    const Symbol *sfn_s  = symtab_intern(e->st, strslice("__sfn", 5));

    /* Parameter list: `__sa<k>` followed by its type annotation, the spelling
     * the reader produces for `x : T`. */
    Form **pv = (Form **)arena_alloc(e->arena, (2 * (n ? n : 1)) * sizeof(Form *));
    Form **cargs = (Form **)arena_alloc(e->arena, (n + 1) * sizeof(Form *));
    cargs[0] = form_sym(e->arena, sp, sfn_s);
    for (uint32_t k = 0; k < n; k++) {
        Type at = (want->as.fn.arg_full_types && want->as.fn.arg_full_types[k])
                      ? *want->as.fn.arg_full_types[k]
                      : type_from_kind(want->as.fn.arg_kinds[k]);
        Form *atf = saffron_seam_type_form(e, &at, sp);
        if (!atf) return NULL;
        char nm[24];
        snprintf(nm, sizeof nm, "__sa%u", k);
        const Symbol *ps = symtab_intern(e->st, strslice(nm, (uint32_t)strlen(nm)));
        pv[2 * k]     = form_sym(e->arena, sp, ps);
        pv[2 * k + 1] = form_type_ann(e->arena, sp, atf);
        cargs[1 + k]  = form_sym(e->arena, sp, ps);
    }
    Type rt = want->as.fn.result_full_type ? *want->as.fn.result_full_type
                                           : type_from_kind(want->as.fn.result_kind);
    /* Built twice on purpose: the same Form is not shared between the return
     * annotation and the `cast` target. */
    Form *rtf_ann  = saffron_seam_type_form(e, &rt, sp);
    Form *rtf_cast = saffron_seam_type_form(e, &rt, sp);
    if (!rtf_ann || !rtf_cast) return NULL;

    Form *dyn_call = form_list(e->arena, sp, cargs, n + 1);
    Form *cast_items[3] = { form_sym(e->arena, sp, cast_s), dyn_call, rtf_cast };
    Form *body = form_list(e->arena, sp, cast_items, 3);
    Form *lam_items[4] = { form_sym(e->arena, sp, fn_s),
                           form_vec(e->arena, sp, pv, 2 * n),
                           form_type_ann(e->arena, sp, rtf_ann),
                           body };
    Form *lam = form_list(e->arena, sp, lam_items, 4);
    Form *bind_items[2] = { form_sym(e->arena, sp, sfn_s), (Form *)arg_form };
    Form *let_items[3] = { form_sym(e->arena, sp, let_s),
                           form_vec(e->arena, sp, bind_items, 2),
                           lam };
    Form *let_form = form_list(e->arena, sp, let_items, 3);

    Type *saved_expected = e->expected_type;
    e->expected_type = NULL;
    Expr *out = elab_form(e, let_form);
    e->expected_type = saved_expected;
    return out;
}

static Expr *saffron_dyn_fn_adaptor_make(Elab *e, Expr *value);
/* r7rs-prelude-procedures-lose-identity: one adaptor per function, not per
 * site.  The adaptor is a non-capturing lambda, which elab_fn lifts to a
 * file-scope function and hands back as an EX_VAR of it; every later boxing of
 * the same function refers to that one, so the emitter's static fat box for
 * it is the procedure's single identity. */
static Expr *saffron_dyn_fn_adaptor(Elab *e, Expr *value) {
    if (!value || value->kind != EX_VAR || !value->as.var.binding ||
        !value->as.var.binding->name || value->type.kind != TY_FN)
        return NULL;
    if (!(e->toplevel_dynamic || lang_span_is_dynamic(value->span))) return NULL;
    Binding *fb = value->as.var.binding;
    /* A local that is an immutable alias of a global function -- `(let ((g
     * car)) ...)` -- is that function: box it through the global's adaptor. */
    if (!fb->is_global && fb->widen_fn_alias && fb->widen_fn_alias->is_global &&
        fb->widen_fn_alias->name && fb->widen_fn_alias->type.kind == TY_FN &&
        type_eq(fb->widen_fn_alias->type, value->type)) {
        Expr *gv = expr_new(e->arena, EX_VAR, value->type, value->span);
        gv->as.var.binding = fb->widen_fn_alias;
        value = gv;
        fb = fb->widen_fn_alias;
    }
    /* One translation unit holds every module's file defs unless this is
     * separate compilation, so a library and the program share the adaptor
     * (SRFI 17's `setter` table is keyed on `car` in the library and asked
     * with `car` from the program).  Under separate compilation the adaptor
     * is a static function of the TU that made it; another module makes its
     * own. */
    if (fb->any_adaptor &&
        (!e->separate_compilation || fb->any_adaptor_module == e->current_module_name)) {
        Expr *v = expr_new(e->arena, EX_VAR, fb->any_adaptor->type, value->span);
        v->as.var.binding = fb->any_adaptor;
        return v;
    }
    Expr *ad = saffron_dyn_fn_adaptor_make(e, value);
    /* Only a global function's adaptor is shared: a local's is a closure over
     * that frame's variable (and may capture), so it is made per site. */
    if (ad && ad->kind == EX_VAR && ad->as.var.binding && fb->is_global &&
        ad->as.var.binding->is_global) {
        fb->any_adaptor = ad->as.var.binding;
        fb->any_adaptor_module = e->current_module_name;
        ad->as.var.binding->is_shared_any_adaptor = true;
    }
    return ad;
}
static Expr *saffron_dyn_fn_adaptor_make(Elab *e, Expr *value) {
    const Type *ft = &value->type;
    if (ft->as.fn.cfnptr || ft->as.fn.arity > TUR_FAT_SHIM_MAX_ARITY) return NULL;
    /* r7rs-lang-plan R6/R7: a VARIADIC function.  Its rest slot is a chain
     * pointer, which the dynamic call packs (emit_dyn_call / the
     * interpreter's EX_DYN_CALL) -- but only for the all-`any` calling
     * convention.  An all-`any` variadic is boxed as itself; a TYPED one with
     * an `any` rest gets an all-`any` VARIADIC adaptor that hands its rest
     * chain through unchanged:
     *   (fn [__da0 ... & __dr : any] : any (NAME __da0 ... (__rest-chain __dr)))
     * `__rest-chain` is the variadic call path's marker for "this is the rest
     * list already" (elab_call.c, AR8).  A typed rest has no such form, so it
     * is boxed as itself and a dynamic call refuses it by its id. */
    if (ft->as.fn.is_variadic) {
        uint32_t nf = ft->as.fn.arity ? ft->as.fn.arity - 1 : 0;
        bool va_all_any = (ft->as.fn.result_kind == TY_ANY);
        for (uint32_t i = 0; i < nf && va_all_any; i++)
            if (ft->as.fn.arg_kinds[i] != TY_ANY) va_all_any = false;
        bool rest_any = ft->as.fn.rest_kind == TY_ANY &&
                        !(ft->as.fn.rest_full_type && ft->as.fn.rest_full_type->kind != TY_ANY);
        if (va_all_any || !rest_any || ft->as.fn.arity == 0) return NULL;
        Span vsp = value->span;
        const Symbol *amp = symtab_intern(e->st, strslice("&", 1));
        const Symbol *anys = symtab_intern(e->st, strslice("any", 3));
        const Symbol *drs = symtab_intern(e->st, strslice("__dr", 4));
        Form **vparams = (Form **)arena_alloc(e->arena, (nf + 3) * sizeof(Form *));
        Form **vcall = (Form **)arena_alloc(e->arena, (nf + 2) * sizeof(Form *));
        vcall[0] = form_sym(e->arena, vsp, value->as.var.binding->name);
        for (uint32_t i = 0; i < nf; i++) {
            char nm[24];
            snprintf(nm, sizeof nm, "__da%u", i);
            const Symbol *ps = symtab_intern(e->st, strslice(nm, (uint32_t)strlen(nm)));
            vparams[i] = form_sym(e->arena, vsp, ps);
            vcall[1 + i] = form_sym(e->arena, vsp, ps);
        }
        vparams[nf]     = form_sym(e->arena, vsp, amp);
        vparams[nf + 1] = form_sym(e->arena, vsp, drs);
        vparams[nf + 2] = form_type_ann(e->arena, vsp, form_sym(e->arena, vsp, anys));
        Form *rc_items[2] = { form_sym(e->arena, vsp, symtab_intern(e->st, strslice("__rest-chain", 12))),
                              form_sym(e->arena, vsp, drs) };
        vcall[1 + nf] = form_list(e->arena, vsp, rc_items, 2);
        Form *vitems[4];
        vitems[0] = form_sym(e->arena, vsp, symtab_intern(e->st, strslice("fn", 2)));
        vitems[1] = form_vec(e->arena, vsp, vparams, nf + 3);
        vitems[2] = form_type_ann(e->arena, vsp, form_sym(e->arena, vsp, anys));
        vitems[3] = form_list(e->arena, vsp, vcall, nf + 2);
        Type *saved_vexp = e->expected_type;
        e->expected_type = NULL;
        Expr *vad = elab_fn(e, form_list(e->arena, vsp, vitems, 4));
        e->expected_type = saved_vexp;
        return vad;
    }
    bool all_any = (ft->as.fn.result_kind == TY_ANY);
    for (uint32_t i = 0; i < ft->as.fn.arity && all_any; i++)
        if (ft->as.fn.arg_kinds[i] != TY_ANY) all_any = false;
    if (all_any) return NULL;
    Span sp = value->span;
    uint32_t n = ft->as.fn.arity;
    Form **params = (Form **)arena_alloc(e->arena, (n ? n : 1) * sizeof(Form *));
    Form **call_items = (Form **)arena_alloc(e->arena, (n + 1) * sizeof(Form *));
    call_items[0] = form_sym(e->arena, sp, value->as.var.binding->name);
    for (uint32_t i = 0; i < n; i++) {
        char nm[24];
        snprintf(nm, sizeof nm, "__da%u", i);
        const Symbol *ps = symtab_intern(e->st, strslice(nm, (uint32_t)strlen(nm)));
        params[i] = form_sym(e->arena, sp, ps);
        call_items[i + 1] = form_sym(e->arena, sp, ps);
    }
    Form *items[3];
    items[0] = form_sym(e->arena, sp, symtab_intern(e->st, strslice("fn", 2)));
    items[1] = form_vec(e->arena, sp, params, n);
    items[2] = form_list(e->arena, sp, call_items, n + 1);
    Form *lam = form_list(e->arena, sp, items, 3);
    Type *saved_expected = e->expected_type;
    e->expected_type = NULL;
    Expr *ad = elab_fn(e, lam);
    e->expected_type = saved_expected;
    return ad;
}

/* Normalise a THIN function value (a bare code pointer: a non-capturing lambda,
 * a `defn` referenced by name) to the fat `{ thunk, env }` representation with
 * an EX_FN_TO_FAT shim; anything else -- already fat, a C function pointer, an
 * arity past the shim table, not a function -- is returned unchanged.
 *
 * The box must not be a per-use malloc.  A `{ shim, orig_fn }` box for a
 * file-scope function is a LINK-TIME CONSTANT -- it depends only on the
 * function -- so the emitter can hoist it to a static, and then the shim
 * allocates nothing at all.  Measured at the `any` widen: without this, 100
 * widens leaked 100 boxes.  Sound for the same reason it is at the `^fat`
 * argument sites that already opt in: a shared box is only wrong if someone
 * frees it, and nothing frees a fn payload.  The emitter applies its own guard
 * (a global EX_VAR that is not a param, closure, poly or already-fat binding)
 * and falls back to the malloc form otherwise, so `static_ok` is a request,
 * not an assertion.
 *
 * Shared by the `any` widen (any-cannot-recover-a-capturing-closure) and, per
 * fn-cell-set-with-capturing-closure-segfaults, by a `^mut` fn cell's init
 * and `set!`. */
Expr *elab_fn_value_to_fat(Elab *e, Expr *value) {
    if (!value) return NULL;
    /* A variable whose BINDING is already fat -- a `^fat` parameter, or a local
     * the elaborator marked fat -- reads thin in its type, so the type test
     * below would wrap the fat handle in a second box whose shim calls the
     * handle as a code pointer: `(set! saved f)` with `f` a `^fat` param, then
     * `(saved 10)`, jumped into the closure's env.  Same classification the
     * tail/join walker uses (repr_of_binding for a param, is_fat otherwise). */
    {
        const Expr *pv = value;
        while (pv && pv->kind == EX_ASCRIBE) pv = pv->as.ascribe_.inner;
        if (pv && pv->kind == EX_VAR && pv->as.var.binding) {
            const Binding *vb = pv->as.var.binding;
            bool fat = vb->is_param
                ? repr_of_binding(vb, REPR_POS_RESULT) == REPR_FAT_HANDLE
                : vb->is_fat;
            if (fat) return value;
        }
    }
    if (value->type.kind == TY_FN && !value->type.as.fn.boxed &&
        !value->type.as.fn.cfnptr && value->type.as.fn.arity <= TUR_FAT_SHIM_MAX_ARITY) {
        Type *bt = (Type *)arena_alloc(e->arena, sizeof(Type));
        *bt = value->type;
        bt->as.fn.boxed = true;
        Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, *bt, value->span);
        shim->as.fn_to_fat_.inner = value;
        shim->as.fn_to_fat_.static_ok = true;
        return shim;
    }
    return value;
}

Expr *elab_coerce_to_any(Elab *e, Expr *value) {
    if (!value) return NULL;
    if (value->type.kind == TY_ANY) return value;  /* already boxed */
    {
        Expr *ad = saffron_dyn_fn_adaptor(e, value);
        if (ad) value = ad;
    }
    /* any-cannot-recover-a-capturing-closure: normalise a FUNCTION payload to
     * the fat `{ thunk, env }` representation before boxing it.
     *
     * An `any` carries one word, and a function reaches it as one of two things:
     * a bare code pointer (a non-capturing lambda, a `defn` referenced by name)
     * or a fat closure box (a capturing lambda, a partial application).  They are
     * called through different protocols, and `cast` emits its call from the
     * TARGET type -- so with two representations in play, one of them is always
     * called wrongly.  Refusing the fat one (which is what the distinct box ids
     * bought) turned a segfault into a panic but left every capturing closure
     * unable to come back out of an `any`.
     *
     * Making the representation uniform removes the choice instead of guarding
     * it: a bare fn is shimmed to fat here, `any_narrow_target` marks a fn target
     * fat to match, and the call head fat-dispatches through slot 0.  Then a
     * lambda, a closure and a partial application all behave the same, which is
     * what the interpreter has always done.
     *
     * A cfnptr is exempt: a `(c-fn ...)` is a real C function pointer with no
     * environment, and fattening it would misrepresent an FFI value.  So is an
     * arity past the shim table, which leaves the payload bare -- it then does
     * not match a fat target, so `is?` is false rather than wrong. */
    value = elab_fn_value_to_fat(e, value);
    Type any_type;
    memset(&any_type, 0, sizeof(any_type));
    any_type.copy_kind = CK_COPY;   /* CK_UNIQUE is 0: a zeroed type is unique-kinded */
    any_type.kind = TY_ANY;

    /* cps-coloring-walk-has-no-arm-for-union-inject, the remaining half: a widen
     * whose operand USES A CONTROL OPERATOR hoists that operand into a let, so
     * the widen sees an ordinary variable.
     *
     * The CPS IR admits `EX_UNION_INJECT` only as a DELEGATABLE node -- atomic
     * operand, or provably control-free -- because delegating a widen over a
     * `call/cc` would direct-emit a control op inside a CPS function. It has no
     * LOWERING for one, so `(defn go [] (call/cc ...))` in a Saffron file, where
     * the `any` return supplies the widen, was evicted `BODY-UNSUPPORTED
     * unsupported form: EX_#100` and the `call/cc` reached the direct emitter,
     * which aborts.
     *
     * The report proposed doing this hoist in the IR and found it blocked: it
     * would need an `Expr` referring to a CPS-bound `CVar`, and no such
     * synthesis exists. Doing it HERE, one layer up, needs no such bridge --
     * the temp is an ordinary binding, the let-init is a control op the lowering
     * already handles, and the widen's operand becomes an `EX_VAR`, which
     * `is_atomic` accepts. Verified by hand first: the same shape written out
     * long-hand (`(let [t (call/cc ...)] (:: t any))`) already compiled and
     * printed 42, with no backend change at all.
     *
     * Note the widen must end up INSIDE the let. Wrapping the let instead --
     * which is what the elaborator produced before this -- leaves the operand a
     * control-bearing `EX_LET` and changes nothing. */
    if (cps_expr_uses_control(value)) {
        LetBinding *lbs = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
        Expr *v = elab_bind_control_temp(e, value, &lbs[0]);
        Expr *inner = expr_new(e->arena, EX_UNION_INJECT, any_type, value->span);
        inner->as.union_inject_.tag_idx = (int64_t)any_box_tag_for_type(&value->type);
        inner->as.union_inject_.value = v;
        Expr *let = expr_new(e->arena, EX_LET, any_type, value->span);
        let->as.let_.bindings = lbs;
        let->as.let_.n = 1;
        let->as.let_.body = inner;
        return let;
    }

    Expr *inject = expr_new(e->arena, EX_UNION_INJECT, any_type, value->span);
    inject->as.union_inject_.tag_idx = (int64_t)any_box_tag_for_type(&value->type);
    inject->as.union_inject_.value = value;
    return inject;
}

/* saffron-effect-row-lost-through-unannotated-call: the RETURN-position widen,
 * with a call hoisted out of the widened body first.
 *
 * An unannotated Saffron `defn` widens its WHOLE BODY here, so
 * `(defn tc [] (if (> (g) 0) 1 0))` handed the entire `if` -- effectful call
 * and all -- to the widen as one operand.  `operand_uses_control` saw no
 * control operator in it, the CPS IR delegated the lot to the direct emitter,
 * and `g`'s direct entry opened a fresh DK root inside the caller's handler.
 * Binding the body to a temp first puts the call at a bind position, where it
 * lowers to the DK-threaded cps->cps edge; the widen then applies to the temp,
 * INSIDE the let, for the reason the control hoist above documents.
 *
 * Deliberately a SEPARATE entry point rather than a branch inside
 * `elab_coerce_to_any`: that helper also serves the CALL-ARGUMENT widen, and
 * three stamps at the argument site key on the coercion returning an
 * `EX_UNION_INJECT` DIRECTLY (`frame_box`, the fat shim's `stack_ok`, and
 * `any_drop_after`).  Wrapping it in a let there silently skipped all three and
 * reinstated exactly the per-widen malloc `any-struct-box-leak-per-widen`
 * removed -- caught by `any-widen-frame-box` and `saffron-dyn-field` under
 * `tests/run-leak-check.sh`, which `tests/run.sh` cannot see (it compiles
 * fixture programs unsanitized).  Return position has no such stamps, so the
 * hoist is confined to it.
 *
 * A NIL-typed body is never hoisted: `void __ctlhoist_N = f(...)` does not
 * compile, and a unit-valued body delivers no word to widen anyway. */
Expr *elab_coerce_to_any_return(Elab *e, Expr *value) {
    if (!value) return NULL;
    if (value->type.kind != TY_ANY && value->type.kind != TY_NIL &&
        e->unit_has_user_effect && !cps_expr_uses_control(value) &&
        saffron_operand_has_call(value)) {
        LetBinding *lbs = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
        Expr *v = elab_bind_control_temp(e, value, &lbs[0]);
        Expr *inner = elab_coerce_to_any(e, v);
        if (inner) {
            Expr *let = expr_new(e->arena, EX_LET, inner->type, value->span);
            let->as.let_.bindings = lbs;
            let->as.let_.n = 1;
            let->as.let_.body = inner;
            return let;
        }
    }
    return elab_coerce_to_any(e, value);
}

/* union-tagged-union-c-emission: the union twin of `elab_coerce_to_any`.
 *
 * A member value flowing into a union slot has to be TAGGED -- the union's C
 * representation is `tur_tagged_t {int64_t tag; int64_t val}`, not the member's
 * own layout.  The call-argument path has done this since IT4, but it is the
 * only position that did: `(let [u (:: (Wide 1 2 3 4) (Wide | int))] ...)` and a
 * `defn` declared `: (Wide | int)` both handed the raw aggregate straight to a
 * `tur_tagged_t` slot, which is not a leak (as the report had it) but a hard cc
 * error -- "invalid initializer" at the let, "incompatible types when returning"
 * at the return.
 *
 * `frame_box` is deliberately NOT set here, in contrast to the argument path.
 * That optimisation rests on the CALLEE provably not retaining the payload for
 * the duration of one call; a value bound to a local or returned outlives the
 * expression that made it, so its box has to be on the heap.  A heap box with
 * no owner is this report's remaining open item -- a leak, which is the safe
 * direction, where a frame box here would be a dangling pointer.
 *
 * Returns `value` unchanged when it is already the union type (no double tag)
 * or when no member matches (the caller's own type check reports that).  */
Expr *elab_coerce_to_union(Elab *e, Expr *value, const Type *union_t) {
    if (!value || !union_t || union_t->kind != TY_UNION) return value;
    if (value->type.kind == TY_UNION) return value;
    for (uint8_t um = 0; um < union_t->as.union_.n_members; um++) {
        const Type *mem = union_t->as.union_.members[um];
        if (!mem || !type_eq(value->type, *mem)) continue;
        Expr *inject = expr_new(e->arena, EX_UNION_INJECT, *union_t, value->span);
        inject->as.union_inject_.tag_idx = (int64_t)um;
        inject->as.union_inject_.value = value;
        return inject;
    }
    return value;
}

/* zero-arg-construct-ground-byvalue-return: true when a parameterised type
 * (`(Option BoundedIdx)`, `(Result Pos cstr)`) carries an element that is a
 * by-value struct/opaque payload (TY_STRUCT) -- the case where the sibling
 * `some`/`ok` constructor already mints a by-value spec (its struct arg trips
 * `emit_abi_register_call`'s `arg_types[i].kind == TY_STRUCT` gate).  A `(Option
 * int)` whose element is the int64 carrier does NOT match, so it stays on the
 * existing carrier+bridge return path; only struct/opaque-element families,
 * where the carrier base would straddle the by-value sibling spec, opt in. */
static bool call_app_has_struct_elem(const Type *t) {
    if (!t || t->kind != TY_APP) return false;
    if (t->as.app.arg && t->as.app.arg->kind == TY_STRUCT) return true;
    return call_app_has_struct_elem(t->as.app.fn);
}

/* security-audit WP5 (M-6): can a value of this type, as the one word it is
 * passed as, BE region memory?  Only a `:heap` node is -- the routed ctor
 * sites allocate nothing else -- and a stdlib collection handle (Vec, Map,
 * Set, MutableMap) is `malloc`'d even though its def is `:heap`.  The
 * elaborator's half of emit_region_word_can_be_node, minus the by-value
 * aggregate arm: an aggregate is not one word, so it cannot ride an `:int`
 * parameter's erasure without changing its representation. */
static bool call_arg_is_region_node_word(Type t) {
    AdtDef *def = NULL;
    if (t.kind == TY_ADT) {
        def = t.as.adt_.def;
    } else if (t.kind == TY_APP) {
        Type args[8];
        uint8_t n_args = 0;
        if (!type_extract_adt_app(&t, &def, args, &n_args)) def = NULL;
    }
    if (!def || !def->is_heap || def->is_opaque) return false;
    return !region_def_is_malloc_collection(def);
}

static bool call_type_has_named_tyvar(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name != NULL;
        case TY_REF_IMMUT: case TY_REF_MUT:
            /* H9: `(& K)` names K through the borrow; a `(& (Vec A))` names A
             * through its recorded full target. */
            return t->as.ref_borrow.target_tyvar != NULL ||
                   call_type_has_named_tyvar(t->as.ref_borrow.target_full);
        case TY_APP:
            return call_type_has_named_tyvar(t->as.app.fn) ||
                   call_type_has_named_tyvar(t->as.app.arg);
        case TY_FN:
            /* poly-closure-inner-dispatch-result-erased (Part 1): a (fn [A] B)
             * param type carries named tyvars in arg_full_types / result_full_type
             * after the re-stamp in type_expr_from_form. */
            if (call_type_has_named_tyvar(t->as.fn.result_full_type)) return true;
            if (t->as.fn.arg_full_types) {
                for (uint32_t i = 0; i < t->as.fn.arity; i++) {
                    if (call_type_has_named_tyvar(t->as.fn.arg_full_types[i])) return true;
                }
            }
            return false;
        case TY_UNION:
            for (uint8_t i = 0; i < t->as.union_.n_members; i++) {
                if (call_type_has_named_tyvar(t->as.union_.members[i])) return true;
            }
            return false;
        case TY_INTERSECTION:
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++) {
                if (call_type_has_named_tyvar(t->as.intersection_.members[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

/* True when `t` mentions a named type variable equal to `name` anywhere in its
 * structure.  Used by the poly-HOF eta-expansion look-ahead to decide whether a
 * sibling bare-tyvar parameter pins a tyvar appearing in a function-typed
 * parameter (e.g. `f : (fn [A] int)` mentions "A", pinned by `a : A`). */
/* forall-dict-pass-multi-constraint-hkt-plan (Task 3.1 residual guard):
 * forward decl -- definition sits just above make_dict_clone. */
static bool dict_clone_dispatch_in_nested_lambda(const Expr *e,
                                                 const ConstraintSet *cs,
                                                 bool inside_lambda);

/* MB2: true if `t` mentions any type variable (a bare TY_TYVAR or a tyvar
 * nested in an application / function type). */
static bool type_mentions_tyvar(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_TYVAR: return true;
        case TY_APP:
            return type_mentions_tyvar(t->as.app.fn) ||
                   type_mentions_tyvar(t->as.app.arg);
        case TY_FN:
            if (type_mentions_tyvar(t->as.fn.result_full_type)) return true;
            if (t->as.fn.arg_full_types)
                for (uint32_t i = 0; i < t->as.fn.arity; i++)
                    if (type_mentions_tyvar(t->as.fn.arg_full_types[i])) return true;
            return false;
        default: return false;
    }
}

static bool type_mentions_tyvar_name(const Type *t, const char *name) {
    if (!t || !name) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name && strcmp(t->as.tyvar_.name, name) == 0;
        case TY_APP:
            return type_mentions_tyvar_name(t->as.app.fn, name) ||
                   type_mentions_tyvar_name(t->as.app.arg, name);
        case TY_FN:
            if (type_mentions_tyvar_name(t->as.fn.result_full_type, name)) return true;
            if (t->as.fn.arg_full_types) {
                for (uint32_t i = 0; i < t->as.fn.arity; i++) {
                    if (type_mentions_tyvar_name(t->as.fn.arg_full_types[i], name)) return true;
                }
            }
            return false;
        case TY_UNION:
            for (uint8_t i = 0; i < t->as.union_.n_members; i++)
                if (type_mentions_tyvar_name(t->as.union_.members[i], name)) return true;
            return false;
        case TY_INTERSECTION:
            for (uint8_t i = 0; i < t->as.intersection_.n_members; i++)
                if (type_mentions_tyvar_name(t->as.intersection_.members[i], name)) return true;
            return false;
        default:
            return false;
    }
}

/* W2: every named tyvar in `t` is absent from each of the call's argument
 * types.  A return-only polymorphic result (e.g. (vec-new) : (Vec A), where A
 * appears in no argument) carries no element-typed content, so it parametrically
 * inhabits a concrete container type at any element type.  A tyvar that DOES
 * flow in through an argument anchors real content and is not free. */
static bool w2_tyvars_free_of_args(const Type *t, Expr **cargs, uint32_t cn) {
    if (!t) return true;
    switch (t->kind) {
        case TY_TYVAR:
            if (t->as.tyvar_.name) {
                for (uint32_t j = 0; j < cn; j++) {
                    if (cargs[j] &&
                        type_mentions_tyvar_name(&cargs[j]->type, t->as.tyvar_.name))
                        return false;
                }
            }
            return true;
        case TY_APP:
            return w2_tyvars_free_of_args(t->as.app.fn, cargs, cn) &&
                   w2_tyvars_free_of_args(t->as.app.arg, cargs, cn);
        default:
            return true;
    }
}

/* W2: true when `arg` is a *return-only polymorphic call result* -- a value
 * whose container/element tyvar is genuinely free because no argument of the
 * call carries it (a bare (vec-new) : (Vec A), (none) : (Option A), etc.).
 * Such a value soundly inhabits the concrete container type at any element
 * type, so forwarding a concrete (Vec int) parameter onto its open tyvar is
 * sound.  A (Vec A) flowing from an abstract parameter (EX_VAR) or from a call
 * argument that itself carries A is NOT free and is excluded here, so a
 * genuinely abstract (Vec A) still fails to match a concrete (Vec int). */
static bool w2_arg_is_free_poly_call(const Expr *arg) {
    if (!arg || arg->kind != EX_CALL) return false;
    if (!call_type_has_named_tyvar(&arg->type)) return false;
    return w2_tyvars_free_of_args(&arg->type,
                                  arg->as.call_.args, arg->as.call_.n_args);
}

/* nullary-generic-call-under-tyvar-expectation / M7: is `name` a type
 * variable quantified by the ENCLOSING signature?  Inside a generic body a
 * tyvar of that name in an argument's type is the body's own abstract `A`,
 * not a fresh one a nullary call minted, and it must stay abstract. */
static bool ng_tyvar_in_sig(const Elab *e, const char *name) {
    if (!e || !name) return false;
    for (uint8_t k = 0; k < e->n_sig_tyvars; k++)
        if (e->sig_tyvars[k] && strcmp(e->sig_tyvars[k], name) == 0) return true;
    return false;
}

static bool ng_type_names_sig_tyvar(const Elab *e, const Type *t) {
    if (!t) return false;
    if (t->kind == TY_TYVAR) return ng_tyvar_in_sig(e, t->as.tyvar_.name);
    if (t->kind == TY_APP)
        return ng_type_names_sig_tyvar(e, t->as.app.fn) ||
               ng_type_names_sig_tyvar(e, t->as.app.arg);
    return false;
}

/* M7 (typed `Cons` tail): an application argument carrying the enclosing
 * body's abstract tyvar -- `(Cons A)` from an ascription or a recursive call
 * inside `(defn rec [A] [(C A)] ...)` -- against the parameter instantiated
 * with the siblings' bindings.  In a constrained body a sibling of type `A`
 * arrives ERASED to the int64 carrier (`(ok-val (:: (one i) (Result A
 * cstr)))` types as `int`), so the binding reads `A := int` and the typed
 * tail's `'A` failed `type_eq` against it: `expected (Cons A), got (Cons A)`.
 * The int carrier IS the abstract `A` here, so an in-scope tyvar on the
 * actual side matches an `int` (or itself) on the instantiated side; every
 * other position must agree exactly. */
static bool ng_erased_match(const Elab *e, const Type *inst, const Type *actual) {
    if (!inst || !actual) return false;
    if (actual->kind == TY_TYVAR && ng_tyvar_in_sig(e, actual->as.tyvar_.name)) {
        if (inst->kind == TY_INT) return true;
        if (inst->kind == TY_TYVAR && inst->as.tyvar_.name && actual->as.tyvar_.name)
            return strcmp(inst->as.tyvar_.name, actual->as.tyvar_.name) == 0;
        return false;
    }
    if (inst->kind == TY_APP && actual->kind == TY_APP)
        return ng_erased_match(e, inst->as.app.fn, actual->as.app.fn) &&
               ng_erased_match(e, inst->as.app.arg, actual->as.app.arg);
    return type_eq(*inst, *actual);
}

static bool call_find_type_binding(CallTypeBinding *bindings, uint8_t n_bindings,
                                   const char *name, uint8_t *out_idx) {
    if (!name) return false;
    for (uint8_t i = 0; i < n_bindings; i++) {
        if (bindings[i].name && strcmp(bindings[i].name, name) == 0) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

/* Walk an applied type down its `fn` spine to the head, returning the head's
 * AdtDef when the spine bottoms out at a TY_ADT (e.g. the head of
 * (type-app (type-app Equal a) b) is the ADT `Equal`). Returns NULL otherwise. */
static const AdtDef *call_app_head_adt(const Type *t) {
    while (t && t->kind == TY_APP) t = t->as.app.fn;
    if (t && t->kind == TY_ADT) return t->as.adt_.def;
    return NULL;
}

/* saffron-lang-plan D5, saffron-unannotated-param-container-cast-panics:
 * rewrite every UNDETERMINED type argument of an applied type to `any`.
 *
 * `vec-get` is `[A] [v : (Vec A) i : int]`, so the seam's target is `(Vec A)`
 * -- a TY_APP whose ARGUMENT is a tyvar.  The seam's own exemption ("a callee
 * expecting a type variable has nothing to check against") is about the type's
 * KIND, so `(Vec A)` sails past it and the seam checks the caller's `any`
 * against a `Vec` instantiation nothing ever chose: the panic
 * `cast: any holds a different instantiation of Vec`.
 *
 * In a Saffron file an undetermined element type IS `any` -- that is D3's
 * default and exactly what the S6 literal widen produces -- so grounding here
 * makes the target `(Vec any)`, which the caller's value satisfies.
 *
 * Walks the whole spine, so a two-parameter head (`(Map K V)`) grounds both,
 * and leaves a DETERMINED argument alone: `(Vec int)` is unchanged, and only a
 * `(Vec int)` value will satisfy it.  Returns true when it changed something,
 * so the caller can tell "grounded" from "already concrete". */
static bool call_ground_open_app_args_to_any(Arena *a, Type *t) {
    if (!t || t->kind != TY_APP) return false;
    bool changed = false;
    if (t->as.app.arg) {
        if (t->as.app.arg->kind == TY_TYVAR) {
            Type *any_t = (Type *)arena_alloc(a, sizeof(Type));
            memset(any_t, 0, sizeof(Type));
            any_t->kind = TY_ANY;
            t->as.app.arg = any_t;
            changed = true;
        } else if (t->as.app.arg->kind == TY_APP) {
            Type inner = *t->as.app.arg;
            if (call_ground_open_app_args_to_any(a, &inner)) {
                Type *slot = (Type *)arena_alloc(a, sizeof(Type));
                *slot = inner;
                t->as.app.arg = slot;
                changed = true;
            }
        }
    }
    if (t->as.app.fn && t->as.app.fn->kind == TY_APP) {
        Type spine = *t->as.app.fn;
        if (call_ground_open_app_args_to_any(a, &spine)) {
            Type *slot = (Type *)arena_alloc(a, sizeof(Type));
            *slot = spine;
            t->as.app.fn = slot;
            changed = true;
        }
    }
    return changed;
}

static void call_collect_tyvar_names(const Type *t, const char **names, uint8_t *n, uint8_t cap);
/* Does `t` name type variables, every one of them quantified by the
 * enclosing signature?  (A type naming none answers false.) */
static bool call_type_tyvars_all_in_sig(const Elab *e, const Type *t) {
    const char *names[16];
    uint8_t n = 0;
    call_collect_tyvar_names(t, names, &n, 16);
    if (n == 0) return false;
    for (uint8_t i = 0; i < n; i++)
        if (!ng_tyvar_in_sig(e, names[i])) return false;
    return true;
}

/* The elaborator whose enclosing signature call_collect_type_bindings may
 * consult (generic-map-assoc-rejects-sig-tyvar-value); set only around the
 * argument check, NULL everywhere else. */
static const Elab *g_call_cb_elab = NULL;

static bool call_collect_type_bindings(const Type *expected, Type actual,
                                       CallTypeBinding *bindings, uint8_t *n_bindings) {
    if (!expected) return true;
    switch (expected->kind) {
        case TY_REF_IMMUT: case TY_REF_MUT: {
            /* saffron-dynamic-surface-pass H9: a borrowed generic parameter
             * `(& K)` binds K from the argument's target -- a borrow's target
             * kind, or the bare value's type when the caller passed it by
             * value.  Without this, `tur-map-kcheck`'s `(& K)` could never
             * contribute K, and an any-held map seamed on argument 0 was
             * grounded to `(Map any any)`. */
            const char *tv = expected->as.ref_borrow.target_tyvar;
            const Type *ef = expected->as.ref_borrow.target_full;
            if (!tv && !ef) return true;
            Type target;
            if (actual.kind == TY_REF_IMMUT || actual.kind == TY_REF_MUT) {
                /* A borrow's target KIND fully names only a scalar.  An
                 * aggregate (`&Pt`, `&String`, `&(Vec int)`) is named by the
                 * full type the borrow recorded, and is bound / compared
                 * through it -- borrowed-aggregate-key-skips-the-key-check:
                 * this arm used to return true for every aggregate, so
                 * nothing checked a borrowed String against the `int` a
                 * `(Map int int)` had bound K to.  An aggregate borrow with
                 * no recorded target (an imported signature, an owning-ref
                 * reborrow) is still accepted unbound, as before. */
                TypeKind tk = actual.as.ref_borrow.target;
                if (actual.as.ref_borrow.target_full) {
                    target = *actual.as.ref_borrow.target_full;
                } else if (typekind_is_numeric(tk) || tk == TY_BOOL || tk == TY_CSTR ||
                           tk == TY_NIL || tk == TY_SYM || tk == TY_PTR_VOID) {
                    target = type_from_kind(tk);
                } else {
                    return true;
                }
            } else {
                target = actual;
            }
            if (!tv) return call_collect_type_bindings(ef, target, bindings, n_bindings);
            Type tvt; memset(&tvt, 0, sizeof tvt);
            tvt.kind = TY_TYVAR; tvt.as.tyvar_.name = tv;
            return call_collect_type_bindings(&tvt, target, bindings, n_bindings);
        }
        case TY_TYVAR: {
            uint8_t idx = 0;
            if (!expected->as.tyvar_.name) return true;
            if (call_find_type_binding(bindings, *n_bindings, expected->as.tyvar_.name, &idx)) {
                /* open-arg-first-binds-call-result-as-int: a binding to a
                 * variable nothing fixed -- an open constructor slot, or the
                 * free result variable of a return-only generic call such as
                 * `(none)` -- is provisional, and a later concrete argument
                 * takes its place.  `(get-or (none) 1.5)` against `[o :
                 * (Option A) d : A]` bound `A` to `(none)`'s own `A` first;
                 * the m5 rule below (meant for the ENCLOSING signature's
                 * variable) then kept it because the names coincided, the
                 * result typed as `int`, and the float spec's 1.5 printed as
                 * `1`.  With any other name for the callee's variable it was
                 * "expected A, got float".  The enclosing signature's own
                 * variables are fixed in each instantiation and keep the m5
                 * treatment. */
                if (bindings[idx].type.kind == TY_TYVAR && actual.kind != TY_TYVAR &&
                    actual.kind != TY_UNKNOWN &&
                    (bindings[idx].type.as.tyvar_.open_slot ||
                     (g_call_cb_elab && bindings[idx].type.as.tyvar_.name &&
                      !ng_tyvar_in_sig(g_call_cb_elab,
                                       bindings[idx].type.as.tyvar_.name)))) {
                    bindings[idx].type = actual;
                    return true;
                }
                /* m5-eq-vec-rewrite-fn-arg-loses-annotation step 2 (fix-i v2):
                 * a prior TYVAR-named binding (from an earlier same-tyvar
                 * actual, e.g. xs:(Vec A) where the outer scope already
                 * names A) accepts a concrete actual at a later arg
                 * position without overwriting the binding.  The binding
                 * stays TYVAR so the emitter's
                 * emit_abi_type_has_concrete_named_tyvar check
                 * (emit_module.c:518) still sees the abstract signal and
                 * routes the call through the relay path.  This unblocks
                 * the gap-1 lambda-after-typed-args shape without the
                 * hamt-delete-regressing binding loss that the reverted
                 * skip-and-upgrade attempt caused. */
                if (bindings[idx].type.kind == TY_TYVAR &&
                    bindings[idx].type.as.tyvar_.name == expected->as.tyvar_.name &&
                    actual.kind != TY_TYVAR) {
                    return true;
                }
                /* generic-map-assoc-rejects-sig-tyvar-value: the same
                 * self-binding (V := V, from `(map-new) : (Map K V)`, whose
                 * slots nothing fixed) against a later argument typed with
                 * the ENCLOSING signature's own variable -- `(map-assoc
                 * (map-new) 1 x)` with `x : A` inside `[A]`.  That variable is
                 * as fixed as a concrete type is in each instantiation, and it
                 * becomes the binding: the call's result then says `(Map K A)`,
                 * as the ascribed `(:: (map-new) (Map int A))` spelling does.
                 * Left as the self-binding, `(map-get ...)` of the result read
                 * back the unfixed V, which collapsed to `int` and retyped the
                 * enclosing generic's declared `A` result -- so at `A := (fn
                 * ...)` its caller thin-called the fat handle it returned. */
                if (bindings[idx].type.kind == TY_TYVAR &&
                    bindings[idx].type.as.tyvar_.name == expected->as.tyvar_.name &&
                    actual.kind == TY_TYVAR && actual.as.tyvar_.name &&
                    actual.as.tyvar_.name != expected->as.tyvar_.name &&
                    g_call_cb_elab && ng_tyvar_in_sig(g_call_cb_elab, actual.as.tyvar_.name)) {
                    bindings[idx].type = actual;
                    return true;
                }
                /* fmap-over-underdetermined-constructor-is-a-defless-shell: an
                 * OPEN slot of the argument -- the `A` of `(Err 3) : (Result A
                 * int)` -- was fixed by nothing, so it agrees with whatever
                 * another argument bound the variable to: `(pick-ok 0.5 (Err
                 * 3))` binds A to float from `d`.  The bare ADT this value used
                 * to be was accepted for any application (KB-022 below). */
                if (actual.kind == TY_TYVAR && actual.as.tyvar_.open_slot)
                    return true;
                /* open-arg-first-binds-call-result-as-int: so does a variable
                 * that is not the enclosing signature's own -- a let-bound
                 * `(none)`'s `A` that nothing grounded: `(let [nn (none)] (cx
                 * 7.1 nn))` against `[d : A o : (Option A)]` was "expected
                 * (Option float), got (Option A)" while the argument-first
                 * order was accepted. */
                if (actual.kind == TY_TYVAR && actual.as.tyvar_.name &&
                    bindings[idx].type.kind != TY_TYVAR && g_call_cb_elab &&
                    !ng_tyvar_in_sig(g_call_cb_elab, actual.as.tyvar_.name))
                    return true;
                return type_eq(bindings[idx].type, actual);
            }
            if (*n_bindings >= 16) return false;
            bindings[*n_bindings].name = expected->as.tyvar_.name;
            bindings[*n_bindings].type = actual;
            (*n_bindings)++;
            return true;
        }
        case TY_APP:
            if (actual.kind != TY_APP || !expected->as.app.fn || !expected->as.app.arg ||
                !actual.as.app.fn || !actual.as.app.arg) {
                /* KB-022: A bare GADT/ADT value (TY_ADT) is a valid argument for a
                 * parameterised parameter type (TY_APP) when their heads agree --
                 * e.g. (Refl) : Equal passed where (Equal a b) is expected. The
                 * value carries no per-position type arguments to refine the named
                 * tyvars, so accept the head match and leave a/b unbound (the
                 * parameter is polymorphic, so any instantiation is sound). */
                if (actual.kind == TY_ADT) {
                    const AdtDef *exp_head = call_app_head_adt(expected);
                    return exp_head && exp_head == actual.as.adt_.def;
                }
                return false;
            }
            {
                /* Ordinary curried match first, on a SCRATCH binding set so a
                 * failed attempt leaves no half-bound tyvars behind.  This is
                 * the existing behaviour and stays exact. */
                CallTypeBinding scratch[16];
                uint8_t n_scratch = *n_bindings;
                for (uint8_t s = 0; s < n_scratch; s++) scratch[s] = bindings[s];
                if (call_collect_type_bindings(expected->as.app.fn, *actual.as.app.fn,
                                               scratch, &n_scratch) &&
                    call_collect_type_bindings(expected->as.app.arg, *actual.as.app.arg,
                                               scratch, &n_scratch)) {
                    for (uint8_t s = 0; s < n_scratch; s++) bindings[s] = scratch[s];
                    *n_bindings = n_scratch;
                    return true;
                }
            }
            /* constrained-hkt-abstract-var-requires-last-param-free: currying
             * can only leave the LAST constructor slot free, so `(m elem)`
             * against `(Result int cstr)` binds `m := (Result int)` -- fixing
             * the very slot meant to stay free -- and then mismatches.  When the
             * expected head is an unbound type VARIABLE, retry with the element
             * at an earlier slot and bind `m` to the hole-headed partial
             * application `(Result _ cstr)`, which saturates back to
             * `(Result elem cstr)` (type_app_fill_hole).
             *
             * Only reached after the curried attempt fails, so every program
             * that type-checked before is unaffected, and the LAST slot keeps
             * priority when more than one could match. */
            if (expected->as.app.fn->kind == TY_TYVAR &&
                expected->as.app.fn->as.tyvar_.name &&
                actual.as.app.fn->kind == TY_APP &&
                actual.as.app.fn->as.app.fn && actual.as.app.fn->as.app.arg &&
                !type_app_has_hole(&actual)) {
                uint8_t ex_idx = 0;
                bool already_bound = call_find_type_binding(
                    bindings, *n_bindings, expected->as.app.fn->as.tyvar_.name, &ex_idx);
                /* slot 0 of a binary application `((C t0) t1)` */
                const Type *t0 = actual.as.app.fn->as.app.arg;
                const Type *ctor = actual.as.app.fn->as.app.fn;
                CallTypeBinding scratch[16];
                uint8_t n_scratch = *n_bindings;
                for (uint8_t s = 0; s < n_scratch; s++) scratch[s] = bindings[s];
                if (call_collect_type_bindings(expected->as.app.arg, *t0,
                                               scratch, &n_scratch)) {
                    Type hole = type_app_hole(tur_type_arena(), *ctor,
                                              *actual.as.app.arg, 0,
                                              (Span){0});
                    if (already_bound) {
                        if (!type_eq(bindings[ex_idx].type, hole)) return false;
                    } else if (n_scratch >= 16) {
                        return false;
                    } else {
                        scratch[n_scratch].name = expected->as.app.fn->as.tyvar_.name;
                        scratch[n_scratch].type = hole;
                        n_scratch++;
                    }
                    for (uint8_t s = 0; s < n_scratch; s++) bindings[s] = scratch[s];
                    *n_bindings = n_scratch;
                    return true;
                }
            }
            return false;
        case TY_FN: {
            /* poly-closure-result-specialization (Stage A1): bind the named
             * tyvars in a function-typed parameter (e.g. `:(fn [A] B)`) from a
             * function-typed argument by recursing structurally over the arg and
             * result full types.  A captureless fn argument carries no
             * arg_full_types/result_full_type (monomorphic), so fall back to the
             * kind-derived shell type for each position.  A fat-boxed closure
             * arrives opaquely as ptr<void>; accept the head and leave the fn's
             * tyvars unbound here -- they bind from the other argument or the
             * call result (same precedent as the TY_ADT-vs-TY_APP head match). */
            if (actual.kind == TY_PTR_VOID) {
                return true;
            }
            if (actual.kind != TY_FN) {
                return type_eq(*expected, actual);
            }
            bool ok = true;
            uint32_t exp_arity = expected->as.fn.arity;
            uint32_t act_arity = actual.as.fn.arity;
            uint8_t n = exp_arity < act_arity ? exp_arity : act_arity;
            for (uint8_t i = 0; i < n; i++) {
                const Type *ea = (expected->as.fn.arg_full_types &&
                                  expected->as.fn.arg_full_types[i])
                    ? expected->as.fn.arg_full_types[i] : NULL;
                if (!ea) continue;  /* concrete arg position -- nothing to bind */
                bool aa_have_full = actual.as.fn.arg_full_types &&
                                    actual.as.fn.arg_full_types[i];
                Type aa = aa_have_full
                    ? *actual.as.fn.arg_full_types[i]
                    : type_from_kind(actual.as.fn.arg_kinds[i]);
                /* van-laarhoven-lens-composition (Gap B2): a nested rank-2 fn-typed
                 * argument (a lens adapter `(fn [p : Point] : (f Point) ...)`) does
                 * not preserve its CONCRETE param full types, so `aa` here is a
                 * def-less kind reconstruction (a bare `Point` shell).  When the
                 * expected position carries no named tyvar (nothing to bind) and the
                 * kinds already agree, skip it rather than `type_eq`-ing a concrete
                 * `Point` against the def-less shell and spuriously failing -- the
                 * concrete-compat check ran at the call's kind level already.  Full
                 * types present on both sides are still compared strictly. */
                if (!aa_have_full && ea->kind == aa.kind &&
                    !call_type_has_named_tyvar(ea))
                    continue;
                ok = ok && call_collect_type_bindings(ea, aa, bindings, n_bindings);
            }
            const Type *er = expected->as.fn.result_full_type;
            if (er) {
                Type ar = actual.as.fn.result_full_type
                    ? *actual.as.fn.result_full_type
                    : type_from_kind(actual.as.fn.result_kind);
                ok = ok && call_collect_type_bindings(er, ar, bindings, n_bindings);
            }
            return ok;
        }
        case TY_UNION:
        case TY_INTERSECTION:
            return type_eq(*expected, actual);
        default:
            return type_eq(*expected, actual);
    }
}

/* van-laarhoven-generic-inference-gap (gap 1): bind an ENCLOSING generic
 * callee's outer type params from a rank-2 (forall-typed) parameter.
 *
 * When a generic function such as
 *   (defn view [S A] [l (forall [f] [(Functor f)] (-> (-> A (f A)) S (f S)))
 *                     s : S] : A ...)
 * is called, the outer tyvar `A` appears ONLY inside the forall-typed parameter
 * `l`.  The plain `call_collect_type_bindings` has no TY_FORALL case (it falls to
 * `type_eq`, which fails between two distinct foralls), so `A` never binds and
 * `view`'s result type stays an abstract tyvar (TUR-E0006).
 *
 * `call_collect_forall_outer_rec` descends the expected forall body against the
 * actual argument's body, collecting bindings for the callee's outer tyvars while
 * treating the forall's OWN bound vars (`f`) as match-anything wildcards that
 * never bind -- so `(f A)` vs `(g int)` pins `A := int` without the quantified `f`
 * (whose name need not match the actual's bound var) polluting or rejecting.
 *
 * It is deliberately PURELY ADDITIVE: it only records outer-tyvar bindings and
 * never rejects the argument (the authoritative arg-type check and the rank-2
 * EX_POLY_WRAP machinery run separately).  So a non-generic callee whose forall
 * param's body mentions only forall-bound vars (e.g. `use-konst`'s
 * `(forall [a] (-> a (-> a a)))`) contributes no bindings and is left untouched. */
static bool call_forall_name_in(const char **names, uint8_t n, const char *name) {
    if (!name || !names) return false;
    for (uint8_t i = 0; i < n; i++)
        if (names[i] && strcmp(names[i], name) == 0) return true;
    return false;
}

static void call_collect_forall_outer_rec(const Type *expected, const Type *actual,
                                          const char **shadow, uint8_t n_shadow,
                                          CallTypeBinding *bindings,
                                          uint8_t *n_bindings) {
    if (!expected || !actual) return;
    switch (expected->kind) {
        case TY_TYVAR: {
            const char *nm = expected->as.tyvar_.name;
            if (!nm) return;
            if (call_forall_name_in(shadow, n_shadow, nm)) return; /* wildcard */
            uint8_t idx;
            if (call_find_type_binding(bindings, *n_bindings, nm, &idx)) return;
            if (actual->kind == TY_TYVAR) return; /* nothing concrete to pin */
            if (*n_bindings >= 16) return;
            bindings[*n_bindings].name = nm;
            bindings[*n_bindings].type = *actual;
            (*n_bindings)++;
            return;
        }
        case TY_APP:
            if (actual->kind != TY_APP || !expected->as.app.fn ||
                !expected->as.app.arg || !actual->as.app.fn || !actual->as.app.arg)
                return;
            call_collect_forall_outer_rec(expected->as.app.fn, actual->as.app.fn,
                                          shadow, n_shadow, bindings, n_bindings);
            call_collect_forall_outer_rec(expected->as.app.arg, actual->as.app.arg,
                                          shadow, n_shadow, bindings, n_bindings);
            return;
        case TY_FN: {
            if (actual->kind != TY_FN) return;
            uint32_t n = expected->as.fn.arity < actual->as.fn.arity
                ? expected->as.fn.arity : actual->as.fn.arity;
            for (uint8_t i = 0; i < n; i++) {
                const Type *ea = expected->as.fn.arg_full_types
                    ? expected->as.fn.arg_full_types[i] : NULL;
                const Type *aa = actual->as.fn.arg_full_types
                    ? actual->as.fn.arg_full_types[i] : NULL;
                if (ea && aa)
                    call_collect_forall_outer_rec(ea, aa, shadow, n_shadow,
                                                  bindings, n_bindings);
            }
            if (expected->as.fn.result_full_type && actual->as.fn.result_full_type)
                call_collect_forall_outer_rec(expected->as.fn.result_full_type,
                                              actual->as.fn.result_full_type,
                                              shadow, n_shadow, bindings, n_bindings);
            return;
        }
        default:
            return;
    }
}

static void call_collect_forall_outer_bindings(const Type *expected, Type actual,
                                               CallTypeBinding *bindings,
                                               uint8_t *n_bindings) {
    if (!expected || expected->kind != TY_FORALL || !expected->as.forall_.body)
        return;
    /* Peel the actual argument's own outer forall (a rank-2 poly value such as
     * `point-x` arrives as a TY_FORALL) so body-vs-body unification lines up. */
    const Type *abody = (actual.kind == TY_FORALL && actual.as.forall_.body)
        ? actual.as.forall_.body : &actual;
    call_collect_forall_outer_rec(expected->as.forall_.body, abody,
                                  expected->as.forall_.var_names,
                                  expected->as.forall_.n_vars,
                                  bindings, n_bindings);
}

static Type call_instantiate_type(Elab *e, const Type *t,
                                  CallTypeBinding *bindings, uint8_t n_bindings) {
    if (!t) return TYPE_UNKNOWN;
    switch (t->kind) {
        case TY_TYVAR: {
            uint8_t idx = 0;
            if (t->as.tyvar_.name &&
                call_find_type_binding(bindings, n_bindings, t->as.tyvar_.name, &idx)) {
                return bindings[idx].type;
            }
            return *t;
        }
        case TY_APP: {
            Type fn = call_instantiate_type(e, t->as.app.fn, bindings, n_bindings);
            Type arg = call_instantiate_type(e, t->as.app.arg, bindings, n_bindings);
            /* constrained-hkt-abstract-var-requires-last-param-free: when the
             * head substituted to a HOLE-headed partial application -- the
             * binding `m := (Result _ cstr)` the call site produced -- applying
             * it must place `arg` at the hole, not curry it onto the end.  So
             * `(m b)` instantiates to `(Result b cstr)`, which is what makes a
             * combinator returning `(m b)` type-check at an ok-biased head. */
            if (type_app_has_hole(&fn))
                return type_app_fill_hole(e->arena, fn, arg, (Span){0});
            return type_app(e->arena, fn, arg, (Span){0});
        }
        case TY_FN: {
            /* poly-combinator-application-element-inference: a callee whose
             * declared RESULT is itself a function type carrying the callee's
             * tyvars -- e.g. `or-parser : (fn [int] (PRes A))` returning a
             * closure over `(PRes A)` -- must have those tyvars substituted so
             * the returned closure's result grounds (`(PRes A)` -> `(PRes int)`)
             * once `A` is bound from the arguments.  Without a TY_FN case here
             * the whole fn type fell through to `default` and came back
             * unchanged, so `(combined 7)` stayed `(PRes A)` and the downstream
             * `match` rejected its arms (`expected tyvar, got int`).  Deep-copy
             * the fn and substitute the full-type arrays, keeping the derived
             * TypeKind shells (arg_kinds/result_kind) in sync with the
             * instantiated payloads. */
            Type ft = *t;
            uint32_t ar = t->as.fn.arity;
            /* ft shares t's out-of-line arg arrays by value; give it private
             * copies before overwriting per-arg kinds for instantiated poly
             * args, so the shared source type is not mutated. */
            if (ar) {
                uint8_t *fk = tur_fn_args_alloc(ar), *ff = tur_fn_args_alloc(ar);
                for (uint32_t k = 0; k < ar; k++) {
                    fk[k] = t->as.fn.arg_kinds[k];
                    ff[k] = t->as.fn.arg_flags[k];
                }
                ft.as.fn.arg_kinds = fk;
                ft.as.fn.arg_flags = ff;
            }
            if (t->as.fn.arg_full_types) {
                Type **afts = (Type **)arena_alloc(e->arena, (ar ? ar : 1) * sizeof(Type *));
                for (uint32_t k = 0; k < ar; k++) {
                    if (t->as.fn.arg_full_types[k]) {
                        afts[k] = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *afts[k] = call_instantiate_type(e, t->as.fn.arg_full_types[k],
                                                         bindings, n_bindings);
                        ft.as.fn.arg_kinds[k] = afts[k]->kind;
                    } else {
                        afts[k] = NULL;
                    }
                }
                ft.as.fn.arg_full_types = afts;
            }
            if (t->as.fn.result_full_type) {
                Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
                *rft = call_instantiate_type(e, t->as.fn.result_full_type,
                                             bindings, n_bindings);
                ft.as.fn.result_full_type = rft;
                ft.as.fn.result_kind = rft->kind;
            }
            return ft;
        }
        case TY_UNION: {
            uint8_t n = t->as.union_.n_members;
            Type **members = (Type **)arena_alloc(e->arena, (n ? n : 1) * sizeof(Type *));
            for (uint8_t i = 0; i < n; i++) {
                members[i] = (Type *)arena_alloc(e->arena, sizeof(Type));
                *members[i] = call_instantiate_type(e, t->as.union_.members[i], bindings, n_bindings);
            }
            return type_union_build(e->arena, members, n);
        }
        case TY_INTERSECTION: {
            uint8_t n = t->as.intersection_.n_members;
            Type **members = (Type **)arena_alloc(e->arena, (n ? n : 1) * sizeof(Type *));
            for (uint8_t i = 0; i < n; i++) {
                members[i] = (Type *)arena_alloc(e->arena, sizeof(Type));
                *members[i] = call_instantiate_type(e, t->as.intersection_.members[i], bindings, n_bindings);
            }
            return type_intersection_build(e->arena, members, n);
        }
        default:
            return *t;
    }
}

static bool type_mentions_tyvar_named(const Type *t, const char *name);

/* D8 Q3: in a Saffron span, a call through a function of type `fnt`
 * ascribes argument `i` to `any` when that parameter is a BARE type variable
 * no compound parameter type mentions -- nothing else can pin it (the
 * argument widen in elab_call_inner).  Shared with the `any` bridge, which
 * must marshal to the convention such a call actually uses. */
bool saffron_bare_tyvar_param_widens(const Type *fnt, uint32_t i) {
    if (!fnt || fnt->kind != TY_FN || !fnt->as.fn.arg_full_types ||
        i >= fnt->as.fn.arity)
        return false;
    const Type *ft = fnt->as.fn.arg_full_types[i];
    if (!ft || ft->kind != TY_TYVAR || !ft->as.tyvar_.name) return false;
    for (uint32_t j = 0; j < fnt->as.fn.arity; j++) {
        const Type *ot = fnt->as.fn.arg_full_types[j];
        if (!ot || j == i || ot->kind == TY_TYVAR) continue;
        if (type_mentions_tyvar_named(ot, ft->as.tyvar_.name)) return false;
    }
    return true;
}

/* The signature a Saffron body calls a function of type `fnt` through: the
 * parameters `saffron_bare_tyvar_param_widens` picks read as `any`, the rest
 * (and the result) as declared. */
Type elab_saffron_call_view(Elab *e, const Type *fnt) {
    Type out = *fnt;
    if (fnt->kind != TY_FN || !fnt->as.fn.arg_full_types || fnt->as.fn.arity == 0)
        return out;
    uint32_t ar = fnt->as.fn.arity;
    Type **afts = (Type **)arena_alloc(e->arena, ar * sizeof(Type *));
    uint8_t *kinds = tur_fn_args_alloc(ar), *flags = tur_fn_args_alloc(ar);
    for (uint32_t k = 0; k < ar; k++) {
        afts[k] = fnt->as.fn.arg_full_types[k];
        kinds[k] = fnt->as.fn.arg_kinds[k];
        flags[k] = fnt->as.fn.arg_flags ? fnt->as.fn.arg_flags[k] : 0;
        if (saffron_bare_tyvar_param_widens(fnt, k)) {
            afts[k] = (Type *)arena_alloc(e->arena, sizeof(Type));
            *afts[k] = type_from_kind(TY_ANY);
            kinds[k] = TY_ANY;
        }
    }
    out.as.fn.arg_full_types = afts;
    out.as.fn.arg_kinds = kinds;
    out.as.fn.arg_flags = flags;
    return out;
}

/* True when `t` is known and names no type variable anywhere in its
 * structure, a function signature's parameter and result slots included. */
bool elab_type_is_ground(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_UNKNOWN:
        case TY_TYVAR:
        case TY_FORALL:
        case TY_EXISTS:
            return false;
        case TY_APP:
            return (!t->as.app.fn || elab_type_is_ground(t->as.app.fn)) &&
                   (!t->as.app.arg || elab_type_is_ground(t->as.app.arg));
        case TY_FN:
            for (uint32_t i = 0; i < t->as.fn.arity; i++) {
                if (t->as.fn.arg_full_types && t->as.fn.arg_full_types[i]) {
                    if (!elab_type_is_ground(t->as.fn.arg_full_types[i]))
                        return false;
                } else if (t->as.fn.arg_kinds &&
                           (t->as.fn.arg_kinds[i] == TY_TYVAR ||
                            t->as.fn.arg_kinds[i] == TY_UNKNOWN)) {
                    return false;
                }
            }
            if (t->as.fn.result_full_type)
                return elab_type_is_ground(t->as.fn.result_full_type);
            return t->as.fn.result_kind != TY_TYVAR &&
                   t->as.fn.result_kind != TY_UNKNOWN;
        default:
            return true;
    }
}

static Type fn_slot_type(const Type *ft, uint32_t k) {
    if (k == ft->as.fn.arity)
        return ft->as.fn.result_full_type ? *ft->as.fn.result_full_type
                                          : type_from_kind(ft->as.fn.result_kind);
    return (ft->as.fn.arg_full_types && ft->as.fn.arg_full_types[k])
               ? *ft->as.fn.arg_full_types[k]
               : type_from_kind(ft->as.fn.arg_kinds[k]);
}

/* static-instance-spec-calls-any-lambda-as-concrete-result and
 * concrete-result-fn-passed-where-an-any-result-fn-is-expected: a function
 * VALUE crossing into a slot whose signature differs from its own only in
 * where `any` appears.
 *
 * `any` is a 16-byte tagged word and every other type is its own register, so
 * `(fn [float] any)` and `(fn [float] float)` are different calling
 * conventions, not a subtype pair: the callee invokes the value through the
 * SLOT's signature and reads the result from the wrong register.  The
 * checker accepts the pair (any is the top type), so the value must be
 * marshalled to the slot's convention instead:
 *
 *   (let [__sfn <arg>]
 *     (fn [__sa0 : W0 ...] : WR (cast? (__sfn (cast? __sa0 H0) ...) WR)))
 *
 * A `W` slot that is `any` where the value's `H` is concrete gets a checked
 * `cast` down to `H`; a concrete `W` where `H` is `any` is widened by the
 * ordinary call.  The result is the same in the other direction: a concrete
 * `WR` over an `any` result is a checked cast, and an `any` `WR` over a
 * concrete one is widened by the `: any` return.
 *
 * `arg` is already elaborated, so it is bound to the temp directly rather
 * than re-elaborated from its form (a lambda literal would otherwise be lifted
 * twice).  Returns NULL -- leave the argument alone -- unless both signatures
 * are ground, the arities match, every slot pair agrees except for `any`, and
 * at least one slot actually differs.  `want` may mention the callee's type
 * variables; `binds` instantiates it first. */
Expr *elab_fn_any_bridge(Elab *e, Expr *arg, const Type *want_decl,
                         const AbiTypeBinding *binds, uint8_t n_binds) {
    if (!e || !arg || !want_decl) return NULL;
    const Type *have = &arg->type;
    if (have->kind != TY_FN || want_decl->kind != TY_FN) return NULL;
    Type want = n_binds ? call_instantiate_type(e, want_decl,
                                                (CallTypeBinding *)binds, n_binds)
                        : *want_decl;
    if (want.kind != TY_FN) return NULL;
    if (have->as.fn.cfnptr || want.as.fn.cfnptr) return NULL;
    if (have->as.fn.is_variadic || want.as.fn.is_variadic) return NULL;
    uint32_t n = want.as.fn.arity;
    if (n != have->as.fn.arity || n > 5) return NULL;
    if (!elab_type_is_ground(&want) || !elab_type_is_ground(have)) return NULL;
    bool differs = false;
    for (uint32_t k = 0; k <= n; k++) {
        Type hs = fn_slot_type(have, k), ws = fn_slot_type(&want, k);
        bool ha = hs.kind == TY_ANY, wa = ws.kind == TY_ANY;
        if (ha != wa) { differs = true; continue; }
        if (!ha && !type_eq(hs, ws)) return NULL;
    }
    if (!differs) return NULL;

    Span sp = arg->span;
    const Symbol *fn_s   = symtab_intern(e->st, strslice("fn", 2));
    const Symbol *cast_s = symtab_intern(e->st, strslice("cast", 4));
    char sfn_nm[40];
    snprintf(sfn_nm, sizeof sfn_nm, "__sfnb_%u", elab_fresh_id(e));
    const Symbol *sfn_s = symtab_intern(e->st, strslice(sfn_nm, (uint32_t)strlen(sfn_nm)));

    Form **pv = (Form **)arena_alloc(e->arena, (2 * (n ? n : 1)) * sizeof(Form *));
    Form **cargs = (Form **)arena_alloc(e->arena, (n + 1) * sizeof(Form *));
    cargs[0] = form_sym(e->arena, sp, sfn_s);
    for (uint32_t k = 0; k < n; k++) {
        Type hs = fn_slot_type(have, k), ws = fn_slot_type(&want, k);
        Form *wtf = saffron_seam_type_form(e, &ws, sp);
        if (!wtf) return NULL;
        char nm[24];
        snprintf(nm, sizeof nm, "__sa%u", k);
        const Symbol *ps = symtab_intern(e->st, strslice(nm, (uint32_t)strlen(nm)));
        pv[2 * k]     = form_sym(e->arena, sp, ps);
        pv[2 * k + 1] = form_type_ann(e->arena, sp, wtf);
        if (ws.kind == TY_ANY && hs.kind != TY_ANY) {
            Form *htf = saffron_seam_type_form(e, &hs, sp);
            if (!htf) return NULL;
            Form *ci[3] = { form_sym(e->arena, sp, cast_s), form_sym(e->arena, sp, ps), htf };
            cargs[1 + k] = form_list(e->arena, sp, ci, 3);
        } else {
            cargs[1 + k] = form_sym(e->arena, sp, ps);
        }
    }
    Type hr = fn_slot_type(have, n), wr = fn_slot_type(&want, n);
    Form *rtf_ann = saffron_seam_type_form(e, &wr, sp);
    if (!rtf_ann) return NULL;
    Form *body = form_list(e->arena, sp, cargs, n + 1);
    if (hr.kind == TY_ANY && wr.kind != TY_ANY) {
        Form *rtf_cast = saffron_seam_type_form(e, &wr, sp);
        if (!rtf_cast) return NULL;
        Form *ci[3] = { form_sym(e->arena, sp, cast_s), body, rtf_cast };
        body = form_list(e->arena, sp, ci, 3);
    }
    Form *lam_items[4] = { form_sym(e->arena, sp, fn_s),
                           form_vec(e->arena, sp, pv, 2 * n),
                           form_type_ann(e->arena, sp, rtf_ann),
                           body };
    Form *lam_form = form_list(e->arena, sp, lam_items, 4);

    Scope inner;
    scope_init(&inner, e->scope);
    e->scope = &inner;
    Binding *sb = binding_new(e, sfn_s, arg->type, false, false, sp);
    scope_add(&inner, sb);
    Type *saved_expected = e->expected_type;
    e->expected_type = NULL;
    Expr *lam = elab_form(e, lam_form);
    e->expected_type = saved_expected;
    e->scope = inner.parent;
    scope_free(&inner);
    if (!lam) return NULL;

    LetBinding *lb = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
    memset(lb, 0, sizeof(*lb));
    lb->binding = sb;
    lb->init = arg;
    Expr *out = expr_new(e->arena, EX_LET, lam->type, sp);
    out->as.let_.bindings = lb;
    out->as.let_.n = 1;
    out->as.let_.body = lam;
    return out;
}

static bool call_reinterpret_kind_is_integral(TypeKind k) {
    switch (k) {
        case TY_BOOL:
        case TY_INT:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
            return true;
        default:
            return false;
    }
}

/* collections-cannot-hold-rc-values item 2: an owning value (rc/weak/ref) has
 * no bit-preserving reinterpretation to the int64 element carrier *as a value*
 * -- but it does as a REFERENCE, provided somebody accounts for the count.  So
 * the carrier crossing is allowed only at sinks that do account for it, and
 * each such sink says which of the two accountings applies:
 *
 *   OWN_CARRY_RETAIN -- crossing mints a new strong reference.  Storing into a
 *     collection (the collection now owns a count it will release on
 *     free/overwrite/removal), or reading one back out (the caller gets its own
 *     count to drop, leaving the collection's intact).
 *   OWN_CARRY_BORROW -- crossing moves the existing reference and mints
 *     nothing.  A compile-time witness the callee discards, or a removal that
 *     transfers the slot's own count out to the caller.
 *   OWN_CARRY_REJECT -- everything else.  Passing the pointer through
 *     unaccounted would leak or double-free depending on which side drops it,
 *     so this stays a diagnostic rather than a silent bit-cast.
 */
typedef enum {
    OWN_CARRY_REJECT = 0,
    OWN_CARRY_BORROW,
    OWN_CARRY_RETAIN,
} OwnCarry;

static OwnCarry own_carry_for_arg(const char *fn, uint32_t idx) {
    if (!fn) return OWN_CARRY_REJECT;
    /* Stores: the slot keeps a strong reference the Vec releases later
     * (stdlib/vec.tur threads bit 1 of the `owned` flag to do it). */
    if (strcmp(fn, "vec-push!") == 0  && idx == 1) return OWN_CARRY_RETAIN;
    if (strcmp(fn, "vec-set-o!") == 0 && idx == 2) return OWN_CARRY_RETAIN;
    /* Type witnesses: the bodies discard the value, so no count changes.
     * `(vec-of x y)` routes every element through tur-vec-homog__ before it
     * ever reaches a push, so rejecting here would reject the literal form. */
    if (strcmp(fn, "tur-vec-homog__") == 0)   return OWN_CARRY_BORROW;
    if (strcmp(fn, "vec-empty-like__") == 0)  return OWN_CARRY_BORROW;
    /* Map inserts, same contract as the Vec stores: the entry keeps a strong
     * reference the map releases when it dies (stdlib/map.tur threads bit 2 of
     * the `owned` flag, which routes the insert through tur_hamt_set_eq_vo with
     * the emitted rc ops). */
    if (strcmp(fn, "map-assoc-eq-o") == 0 && idx == 3) return OWN_CARRY_RETAIN;
    /* option-rc-payload-constructible-only-from-inline-c: the SUM
     * CONSTRUCTORS.  `(some x)` / `(ok x)` / `(err x)` wrap the value in a
     * result the CALLER receives and owns -- there is no second, longer-lived
     * holder the way a collection is one, so the reference MOVES in and
     * nothing is minted.  That is OWN_CARRY_BORROW by this enum's own
     * definition ("crossing moves the existing reference and mints nothing").
     *
     * RETAIN would be wrong in the other direction: nothing releases an
     * Option's payload when the value goes out of scope, so a count minted
     * here would never come back down.  BORROW keeps exactly one owner, which
     * is the contract `weak/upgrade` and `weak/unwrap` already document ("the
     * caller owns it and must rc/drop it").
     *
     * Without these rows the default REJECT fired on a construction with no
     * collection anywhere in the program -- so `(Option rc<A>)` could be
     * RETURNED, built in inline C and matched, but not built in Turmeric.  The
     * inline-C form is the one that shipped, and it leaked a box per call
     * until 2026-08-30, so the language was rejecting the safe spelling and
     * accepting the unsafe one. */
    if (strcmp(fn, "some") == 0 && idx == 0) return OWN_CARRY_BORROW;
    if (strcmp(fn, "ok")   == 0 && idx == 0) return OWN_CARRY_BORROW;
    if (strcmp(fn, "err")  == 0 && idx == 0) return OWN_CARRY_BORROW;
    return OWN_CARRY_REJECT;
}

/* An argument that MINTS its own reference hands that reference to the sink;
 * retaining on top of it would leave the collection holding two counts and
 * releasing one.  `(rc/of ...)` and `(rc/clone ...)` are the two forms that
 * provably do this, and neither result is tracked by the caller (there is no
 * binding to drop it), so the sink takes the fresh reference as-is.
 *
 * Everything else is treated as a BORROW and retained.  That is the safe
 * default of the two: an unnecessary retain leaks, a missing one double-frees,
 * and a bare `(vec-push! v a)` -- where `a` keeps its own count and drops it at
 * scope exit -- is by far the common shape. */
static bool own_arg_mints_reference(const Expr *a) {
    while (a && a->kind == EX_ASCRIBE) a = a->as.ascribe_.inner;
    return a && (a->kind == EX_RC_OF || a->kind == EX_RC_CLONE);
}

static OwnCarry own_carry_for_result(const char *fn) {
    if (!fn) return OWN_CARRY_REJECT;
    if (strcmp(fn, "vec-get") == 0)  return OWN_CARRY_RETAIN;
    if (strcmp(fn, "vec-pop!") == 0) return OWN_CARRY_BORROW;
    /* A map read hands the caller its own reference, like vec-get: the entry
     * keeps the map's, so the value read out must be counted separately. */
    if (strcmp(fn, "map-get-eq-o") == 0) return OWN_CARRY_RETAIN;
    return OWN_CARRY_REJECT;
}

static Expr *call_wrap_reinterpret_owning(Elab *e, Expr *inner, TypeKind target_kind,
                                          Span span, OwnCarry carry);

static Expr *call_wrap_reinterpret(Elab *e, Expr *inner, TypeKind target_kind, Span span) {
    return call_wrap_reinterpret_owning(e, inner, target_kind, span, OWN_CARRY_REJECT);
}

static Expr *call_wrap_reinterpret_owning(Elab *e, Expr *inner, TypeKind target_kind,
                                          Span span, OwnCarry carry) {
    if (!inner) return NULL;
    TypeKind source_kind = inner->type.kind;
    if (source_kind == target_kind) return inner;
    /* Owning kinds are handled by the OwnCarry rules above, ahead of the
     * size-based scalar reinterpretation (an rc is a control-block pointer, so
     * the bits carry fine -- it is the count that needs a decision). */
    bool owning_src = source_kind == TY_RC || source_kind == TY_WEAK ||
                      source_kind == TY_REF || source_kind == TY_LREF;
    bool owning_dst = target_kind == TY_RC || target_kind == TY_WEAK ||
                      target_kind == TY_REF || target_kind == TY_LREF;
    if (owning_src || owning_dst) {
        if (carry == OWN_CARRY_REJECT) {
            /* Historically this was a bare `tur: emit: invalid EX_REINTERPRET
             * rc -> int` abort with no span, from deep in codegen, for an
             * ordinary program like `(vec-of (rc/clone a))`.  This is the last
             * point that still has a span, so the rejection belongs here.
             * (The variadic rest-arg check is NOT the hook: `vec-of` is a macro
             * expanding to `vec-push!` calls, so it never reaches that path.) */
            diag_emit(DIAG_ERROR, span,
                      "cannot store an owning value (%s) in a collection: elements "
                      "go through an int64 carrier that cannot hold a reference the "
                      "collection would have to own. Store a plain handle, or keep "
                      "the value outside the collection",
                      typekind_to_string(owning_src ? source_kind : target_kind));
            /* option-rc-payload-constructible-only-from-inline-c, fix
             * direction 2.  The line above names a COLLECTION, which is where
             * this fires most often and reads correctly there -- but the check
             * is really about crossing a GENERIC boundary, so it also fires
             * with no collection anywhere in the program (`(unwrap o)` over an
             * `(Option rc<A>)`).  Rather than reword a message that is right
             * for its common case, say what the rule actually is and name the
             * spellings that work. */
            diag_emit(DIAG_NOTE, span,
                      "the restriction is about crossing a GENERIC boundary, "
                      "not about collections as such: a tyvar parameter or "
                      "result is erased to the int64 carrier, which has no "
                      "room for the reference count");
            diag_emit(DIAG_NOTE, span,
                      "the sum constructors are allowed -- `(some x)` / "
                      "`(ok x)` / `(err x)` MOVE the reference into a value "
                      "the caller owns.  To read an owning payload back out, "
                      "destructure with `match` rather than the generic "
                      "accessor, as stdlib's `weak/unwrap` does");
            return NULL;
        }
        /* Only rc<T> is refcount-accounted today.  A weak/ref crossing has no
         * count to take, so it would be a bare pointer in a slot nobody owns --
         * still the leak/double-free the diagnostic above describes. */
        if ((owning_src && source_kind != TY_RC) ||
            (owning_dst && target_kind != TY_RC)) {
            diag_emit(DIAG_ERROR, span,
                      "cannot store an owning value (%s) in a collection: only "
                      "rc<T> elements are reference-counted through the int64 "
                      "carrier. Store an rc<T>, a plain handle, or keep the "
                      "value outside the collection",
                      typekind_to_string(owning_src ? source_kind : target_kind));
            return NULL;
        }
        Expr *own = expr_new(e->arena, EX_REINTERPRET, type_from_kind(target_kind), span);
        own->as.reinterpret_.expr = inner;
        own->as.reinterpret_.source_kind = source_kind;
        own->as.reinterpret_.target_kind = target_kind;
        own->as.reinterpret_.retain = (carry == OWN_CARRY_RETAIN);
        return own;
    }
    int src_size = type_size_bytes(source_kind);
    int dst_size = type_size_bytes(target_kind);
    if (src_size <= 0 || dst_size <= 0) return inner;
    /* decode-bool-carrier-instance-ascription: a polymorphic call whose result
     * is the int64 carrier but whose declared return resolves to a sub-word
     * integral scalar (bool, int8/16/32) still needs to be re-typed at the
     * elab level so `(ok-val (:: ... (Result bool cstr)))` is `bool`, not
     * `int`. Emit lowers size-mismatched integral pairs as plain C casts;
     * same-size pairs keep the bit-preserving union trick (still correct for
     * float<->int and cstr<->int). Mixing float with an integer at different
     * sizes is neither bit- nor value-meaningful, so bail in that case. */
    if (src_size != dst_size) {
        /* float32-generic-call-result-printed-as-carrier: the int64 CARRIER
         * holding float32 bits is the one mixed-size float pair that IS
         * bit-meaningful -- the producing side already stores the low 4 bytes
         * through the union overlay (`emit_carrier_bridge`'s inline arm covers
         * TY_FLOAT32), so the read-back is the same overlay in reverse.
         * Silently returning `inner` here was the root cause of that report:
         * the tyvar-result path REQUESTED the reinterpret (`A := float32`
         * collected fine) and this bail dropped it, so the call stayed typed
         * `int` and printed 1088631603 -- while the float64 twin, being
         * same-size, took the union arm and worked.  Note the bail was
         * SILENT: the caller cannot tell a wrapped result from a dropped
         * wrap, which is what made this an elaboration bug that read like an
         * emit bug. */
        bool carrier_f32 =
            (source_kind == TY_INT && target_kind == TY_FLOAT32) ||
            (source_kind == TY_FLOAT32 && target_kind == TY_INT);
        if (!carrier_f32 &&
            (!call_reinterpret_kind_is_integral(source_kind) ||
             !call_reinterpret_kind_is_integral(target_kind))) {
            return inner;
        }
    }
    Expr *out = expr_new(e->arena, EX_REINTERPRET, type_from_kind(target_kind), span);
    out->as.reinterpret_.expr = inner;
    out->as.reinterpret_.source_kind = source_kind;
    out->as.reinterpret_.target_kind = target_kind;
    return out;
}

/* Phase P3: HAMT lowering - create a call to a HAMT function binding */
static Expr *elab_call_hamt_fn(Elab *e, Span span, const Symbol *fn_name, uint32_t n_args, Expr **args) {
    /* Look up the HAMT function binding */
    bool fn_qual_err = false;
    Binding *fn_binding = elab_lookup_sym(e, fn_name, span, &fn_qual_err);
    if (!fn_binding && fn_qual_err) return NULL;
    if (!fn_binding) {
        /* HAMT module not imported - for now, we require it to be imported */
        diag_emit(DIAG_ERROR, span, "HAMT module must be imported to use persistent maps");
        return NULL;
    }
    
    /* Create the EX_CALL expression with the function's return type */
    Type result_type;
    if (fn_binding->type.kind == TY_FN) {
        result_type = type_from_kind(fn_binding->type.as.fn.result_kind);
    } else {
        result_type = TYPE_NIL;
    }
    Expr *out = expr_new(e->arena, EX_CALL, result_type, span);
    out->as.call_.fn_binding = fn_binding;
    out->as.call_.args = args;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    return out;
}

/* closure-drop-glue S1: hoist an INLINE capturing closure passed to a `^borrow`
 * (FA_BORROW) fn-param into a fresh let-binding, so the direct emitter's
 * scoped-env free (let_binding_env_freeable + the FA_BORROW non-escape relaxation
 * in binding_escapes_impl) reclaims its heap env at scope exit -- a borrowed
 * closure is invoked but not retained by the callee, so it dies at this call.
 * Without a let binding the inline env has no name for the scope-exit free to
 * target and leaks.  Returns the (possibly let-wrapped) expression; a no-op when
 * `call` is not an EX_CALL to a fn with a FA_BORROW inline-closure arg. */
/* closure-drop-glue S1: is argument `a` (ascribe-peeled) to parameter `i` of
 * `fb` a fresh, uniquely-owned closure env whose heap allocation can be freed at
 * the call scope's exit?  True when the parameter does not retain it (a `^borrow`
 * param, or an inferred non-retaining fn-param) AND the argument is either an
 * inline capturing EX_CLOSURE literal (S1.2) or a call to a fresh-closure-
 * returning fn (S1c -- the make-scaler shape).  Both produce a fresh env the
 * caller uniquely owns; the non-retention guarantees the callee will not keep it
 * past the call. */
static bool arg_is_freeable_closure_source(const Binding *fb, uint32_t i,
                                           const Expr *a) {
    if (!a) return false;
    bool nonretain = FN_ARG_FLAG(fb->type.as.fn, i, FA_BORROW)
                     || (i < 32 && (fb->nonretain_param_mask & (1u << i)));
    if (!nonretain) return false;
    if (a->kind == EX_CLOSURE && a->as.closure_.closure
        && a->as.closure_.closure->n_captures > 0)
        return true;
    if (a->kind == EX_CALL && a->as.call_.fn_binding
        && a->as.call_.fn_binding->returns_fresh_closure)
        return true;
    /* cps-capturing-closure-env-leaks-through-dyn-call: the dynamic twins --
     * a capturing lambda widened to `any` for an `any` parameter, and a call
     * to a function returning one.  Hoisted into a let, the `any` scope drop
     * (direct) or the entry-boundary reap (CPS) releases the env. */
    if (expr_is_fresh_any_closure(a))
        return true;
    return false;
}

static Expr *hoist_borrowed_closure_args(Elab *e, Expr *call, Span span) {
    if (!call || call->kind != EX_CALL) return call;
    const Binding *fb = call->as.call_.fn_binding;
    if (!fb || fb->type.kind != TY_FN || !fb->type.as.fn.arg_flags) return call;
    Expr **args = call->as.call_.args;
    uint32_t n_args = call->as.call_.n_args;
    uint32_t n_hoist = 0;
    /* RM1 (bind chains): a continuation into a poly / fat slot arrives under
     * an EX_POLY_WRAP / EX_FN_TO_FAT / EX_POLY_TO_FAT; the closure to hoist is
     * the wrap's INNER, and the wrap stays in place around the hoisted var so
     * the call site still packs it the way the slot expects. */
#define HOIST_PEEL_SLOT(slot_)                                                  \
    do {                                                                       \
        for (;;) {                                                             \
            Expr *_x = *(slot_);                                               \
            if (!_x) break;                                                    \
            if (_x->kind == EX_ASCRIBE) (slot_) = &_x->as.ascribe_.inner;      \
            else if (_x->kind == EX_POLY_WRAP) (slot_) = &_x->as.poly_wrap_.inner; \
            else if (_x->kind == EX_FN_TO_FAT) (slot_) = &_x->as.fn_to_fat_.inner; \
            else if (_x->kind == EX_POLY_TO_FAT) (slot_) = &_x->as.poly_to_fat_.inner; \
            else break;                                                        \
        }                                                                      \
    } while (0)
    for (uint32_t i = 0; i < n_args && i < fb->type.as.fn.arity; i++) {
        Expr **slot = &args[i];
        HOIST_PEEL_SLOT(slot);
        if (arg_is_freeable_closure_source(fb, i, *slot))
            n_hoist++;
    }
    if (n_hoist == 0) return call;
    LetBinding *lbs = (LetBinding *)arena_alloc(e->arena, n_hoist * sizeof(LetBinding));
    uint32_t h = 0;
    for (uint32_t i = 0; i < n_args && i < fb->type.as.fn.arity; i++) {
        Expr **slot = &args[i];
        HOIST_PEEL_SLOT(slot);
        Expr *a = *slot;
        if (!arg_is_freeable_closure_source(fb, i, a))
            continue;
        char nm[48];
        snprintf(nm, sizeof nm, "__borrowc_%u", elab_fresh_id(e));
        const Symbol *sym = symtab_intern(e->st, strslice(nm, (uint32_t)strlen(nm)));
        Binding *cb = binding_new(e, sym, a->type, false, false, span);
        /* fn-value-fat-normalization (effect-row increment): record the lifted
         * lambda behind this hoist temp so the CPS coloring / threadability
         * walks resolve the temp back to the lambda (without this, a capturing
         * callback into an effectful param reads as an opaque var, is never
         * force-colored or threadable, and perm-taints its effect row to the
         * deleted fiber engine).  Deliberately NOT closure_fn_binding, which
         * carries direct-call semantics at emit. */
        if (a->kind == EX_CLOSURE && a->as.closure_.closure &&
            a->as.closure_.closure->fn)
            cb->hoist_closure_fn_binding = a->as.closure_.closure->fn->binding;
        lbs[h].binding = cb;
        lbs[h].init = a;                 /* ascribe-peeled EX_CLOSURE literal or a
                                          * fresh-closure-returning call */
        h++;
        Expr *v = expr_new(e->arena, EX_VAR, a->type, span);
        v->as.var.binding = cb;
        *slot = v;                       /* the call (or its wrap) now references the binding */
    }
#undef HOIST_PEEL_SLOT
    Expr *let = expr_new(e->arena, EX_LET, call->type, span);
    let->as.let_.bindings = lbs;
    let->as.let_.n = n_hoist;
    let->as.let_.body = call;
    return let;
}

/* Phase P3: HAMT lowering - lower map function calls when first arg is persistent */
static Expr *elab_lower_map_call(Elab *e, const Form *call, const Symbol *name) {
    uint32_t n_args = call->as.list.len - 1;
    
    /* Elaborate the first argument to check if it's a persistent binding */
    /* For map-new, there are no arguments, so we skip the first arg check */
    if (n_args == 0) {
        /* Only map-new takes 0 arguments */
        if (name != e->sym_map_new) {
            diag_emit(DIAG_ERROR, call->span, "map function '%s' requires at least 1 argument", name->name);
            return NULL;
        }
        /* Phase P3: if we are elaborating the RHS of a ^persistent let binding,
         * lower map-new directly to hamt/new so the result type is void * and
         * subsequent HAMT operations (count, assoc, …) receive the right type. */
        if (e->in_persistent_let) {
            e->needs_hamt = true;
            extern bool g_needs_hamt;
            g_needs_hamt = true;
            return elab_call_hamt_fn(e, call->span, e->sym_hamt_new, 0, NULL);
        }
    }
    
    Expr *first_arg = NULL;
    if (n_args > 0) {
        first_arg = elab_form(e, call->as.list.items[1]);
        if (!first_arg) return NULL;
    }
    
    /* Check if first argument is a variable reference to a persistent binding */
    /* For map-new, first_arg is NULL, so we treat it as non-persistent (it creates a new map) */
    bool is_persistent_map = false;
    if (first_arg && first_arg->kind == EX_VAR && first_arg->as.var.binding->is_persistent) {
        is_persistent_map = true;
    }
    
    if (!is_persistent_map) {
        /* Not a persistent binding - fall through to normal elaboration */
        /* Re-elaborate all args together */
        Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
        if (n_args > 0) {
            args[0] = first_arg;
            for (uint32_t i = 1; i < n_args; i++) {
                args[i] = elab_form(e, call->as.list.items[1 + i]);
                if (!args[i]) return NULL;
            }
        }
        
        /* Look up the function binding */
        bool fn_qual_err = false;
        Binding *fn_binding = elab_lookup_sym(e, name, call->as.list.items[0]->span, &fn_qual_err);
        if (!fn_binding && fn_qual_err) return NULL;
        if (!fn_binding) {
            diag_emit(DIAG_ERROR, call->span, "unknown function '%s'", name->name);
            return NULL;
        }
        return elab_call_fn(e, call, fn_binding);
    }
    
    /* Mark that we need HAMT */
    e->needs_hamt = true;
    /* Phase P3: Set global flag for emit phase */
    extern bool g_needs_hamt;
    g_needs_hamt = true;
    
    /* Elaborate remaining arguments (for map-new, n_args is 0, so this is safe) */
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    if (n_args > 0) {
        args[0] = first_arg;
        for (uint32_t i = 1; i < n_args; i++) {
            args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (!args[i]) return NULL;
        }
    }
    
    /* Transform based on the function name */
    if (name == e->sym_map_new) {
        if (n_args != 0) {
            diag_emit(DIAG_ERROR, call->span, "map-new takes 0 arguments");
            return NULL;
        }
        /* map-new -> (hamt/new) */
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_new, 0, NULL);
    } else if (name == e->sym_assoc) {
        if (n_args != 3) {
            diag_emit(DIAG_ERROR, call->span, "assoc takes 3 arguments: (assoc map key value)");
            return NULL;
        }
        /* :cstr keys hash and compare by CONTENT (hamt/set-cstr), never by
         * pointer -- identity hash/compare of string keys only "worked" when
         * the C compiler merged identical literals (unspecified, C11
         * 6.4.5p7), and never for runtime-built keys.  args is already
         * {m, k, v}, the wrapper's exact signature. */
        if (args[1]->type.kind == TY_CSTR)
            return elab_call_hamt_fn(e, call->span, e->sym_hamt_set_cstr, 3, args);
        /* assoc m k v -> (hamt/set m (hamt_hash_ptr k) k v) */
        /* First, compute the hash: (hamt_hash_ptr k) */
        bool hash_qual_err = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err);
        if (!hash_binding && hash_qual_err) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        /* Create the hash argument */
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];  /* key */
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        /* Create args for hamt/set: m, hash, k, v */
        Expr **set_args = (Expr **)arena_alloc(e->arena, 4 * sizeof(Expr *));
        set_args[0] = args[0];  /* m */
        set_args[1] = hash_call;  /* hash */
        set_args[2] = args[1];  /* k */
        set_args[3] = args[2];  /* v */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_set, 4, set_args);
    } else if (name == e->sym_dissoc) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "dissoc takes 2 arguments: (dissoc map key)");
            return NULL;
        }
        /* :cstr keys: content hash + content compare (see the assoc arm). */
        if (args[1]->type.kind == TY_CSTR)
            return elab_call_hamt_fn(e, call->span, e->sym_hamt_del_cstr, 2, args);
        /* dissoc m k -> (hamt/del m (hamt_hash_ptr k) k) */
        bool hash_qual_err2 = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err2);
        if (!hash_binding && hash_qual_err2) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        Expr **del_args = (Expr **)arena_alloc(e->arena, 3 * sizeof(Expr *));
        del_args[0] = args[0];  /* m */
        del_args[1] = hash_call;  /* hash */
        del_args[2] = args[1];  /* k */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_del, 3, del_args);
    } else if (name == e->sym_map_get) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "get takes 2 arguments: (get map key)");
            return NULL;
        }
        /* :cstr keys: content hash + content compare (see the assoc arm). */
        if (args[1]->type.kind == TY_CSTR)
            return elab_call_hamt_fn(e, call->span, e->sym_hamt_get_cstr, 2, args);
        /* get m k -> (hamt/get m (hamt_hash_ptr k) k) */
        bool hash_qual_err4 = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err4);
        if (!hash_binding && hash_qual_err4) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        Expr **get_args = (Expr **)arena_alloc(e->arena, 3 * sizeof(Expr *));
        get_args[0] = args[0];  /* m */
        get_args[1] = hash_call;  /* hash */
        get_args[2] = args[1];  /* k */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_get, 3, get_args);
    } else if (name == e->sym_map_has) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "has? takes 2 arguments: (has? map key)");
            return NULL;
        }
        /* :cstr keys: content hash + content compare (see the assoc arm). */
        if (args[1]->type.kind == TY_CSTR)
            return elab_call_hamt_fn(e, call->span, e->sym_hamt_has_cstr, 2, args);
        /* has? m k -> (hamt/has? m (hamt_hash_ptr k) k) */
        bool hash_qual_err3 = false;
        Binding *hash_binding = elab_lookup_sym(e, e->sym_hamt_hash_ptr, call->span, &hash_qual_err3);
        if (!hash_binding && hash_qual_err3) return NULL;
        if (!hash_binding) {
            diag_emit(DIAG_ERROR, call->span, "HAMT module must be imported to use persistent maps");
            return NULL;
        }
        
        Expr **hash_args = (Expr **)arena_alloc(e->arena, 1 * sizeof(Expr *));
        hash_args[0] = args[1];
        Expr *hash_call = expr_new(e->arena, EX_CALL, (hash_binding->type.kind == TY_FN ? type_from_kind(hash_binding->type.as.fn.result_kind) : TYPE_NIL), call->span);
        hash_call->as.call_.fn_binding = hash_binding;
        hash_call->as.call_.args = hash_args;
        hash_call->as.call_.n_args = 1;
        hash_call->as.call_.fn_expr = NULL;
        
        Expr **has_args = (Expr **)arena_alloc(e->arena, 3 * sizeof(Expr *));
        has_args[0] = args[0];  /* m */
        has_args[1] = hash_call;  /* hash */
        has_args[2] = args[1];  /* k */
        
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_has, 3, has_args);
    } else if (name == e->sym_map_count) {
        if (n_args != 1) {
            diag_emit(DIAG_ERROR, call->span, "count takes 1 argument: (count map)");
            return NULL;
        }
        /* count m -> (hamt/count m) */
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_count, 1, args);
    } else if (name == e->sym_map_merge) {
        if (n_args != 2) {
            diag_emit(DIAG_ERROR, call->span, "merge takes 2 arguments: (merge a b)");
            return NULL;
        }
        /* merge a b -> (hamt/merge a b) */
        return elab_call_hamt_fn(e, call->span, e->sym_hamt_merge, 2, args);
    }
    
    diag_emit(DIAG_ERROR, call->span, "unexpected map function '%s'", name->name);
    return NULL;
}

/* saffron-lang-plan S4/D4 (G5): build the dynamic call `(fnv args...)` whose
 * callee is a runtime `any` value.  Resolution moves to runtime, where the
 * value's own tag says whether it is callable and with what arity.  Every
 * argument crosses as a BOX -- the callee is invoked through
 * `TUR_APPLY*_T(tur_tagged_t, ...)` -- so a concrete one is widened here
 * (saffron-dyn-call-concrete-argument-is-not-widened: `(f 41)` with `f : any`
 * once emitted an `int64_t` into a `tur_tagged_t` slot).
 *
 * Shared by the two shapes of head: a bare symbol bound to an `any`
 * (`(f h)`, the common one) and, per saffron-dynamic-surface-pass (low), an
 * arbitrary EXPRESSION of type `any` -- `((adder 1) 1)`, `((mkc) 7)` -- which
 * used to be "expression in call head has type any, which is not callable".
 * The node's `fn` slot has always taken any expression (emit_value /
 * eval_expr evaluate it, the CPS and effect walks descend into it), so the
 * second shape needs no new machinery, only the permission the first already
 * had. */
/* True when an enclosing expectation is an applied type that fixes at least
 * one CONCRETE type argument -- the signal the Saffron ctor / generic-fn widens
 * defer to.  An application whose arguments are all `any` (or a NULL / non-app
 * expectation) pins nothing.  Walks the curried TY_APP chain; a hole-headed
 * partial application has no free slot to inspect and counts as pinning. */
static bool saffron_expected_app_pins(const Type *t) {
    if (!t || t->kind != TY_APP) return false;
    const Type *cur = t;
    while (cur && cur->kind == TY_APP) {
        if (type_app_has_hole(cur)) return true;
        if (!cur->as.app.arg || cur->as.app.arg->kind != TY_ANY) return true;
        cur = cur->as.app.fn;
    }
    return false;
}

static Expr *saffron_dyn_call_on(Elab *e, const Form *call, Expr *fnv) {
    uint32_t dn = call->as.list.len - 1;
    Expr **dargs = (dn == 0) ? NULL
        : (Expr **)arena_alloc(e->arena, dn * sizeof(Expr *));
    for (uint32_t i = 0; i < dn; i++) {
        dargs[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!dargs[i]) return NULL;
        if (dargs[i]->type.kind != TY_ANY && dargs[i]->type.kind != TY_NEVER)
            dargs[i] = elab_coerce_to_any(e, dargs[i]);
    }
    Type any_t;
    memset(&any_t, 0, sizeof(any_t));
    any_t.copy_kind = CK_COPY;   /* CK_UNIQUE is 0: a zeroed type is unique-kinded (saffron-any-let-binding-is-unique) */
    any_t.kind = TY_ANY;
    Expr *dc = expr_new(e->arena, EX_DYN_CALL, any_t, call->span);
    dc->as.dyn_call_.fn     = fnv;
    dc->as.dyn_call_.args   = dargs;
    dc->as.dyn_call_.n_args = dn;
    return elab_hoist_control_operands(e, dc);
}

static Expr *elab_call_head_expr(Elab *e, const Form *call, Expr *head_expr) {
    TypeKind head_kind = head_expr->type.kind;
    if (head_kind == TY_ANY &&
        (lang_span_is_dynamic(call->span) || e->toplevel_dynamic))
        return saffron_dyn_call_on(e, call, head_expr);
    if (head_kind != TY_FN && head_kind != TY_PTR_VOID && head_kind != TY_CONT) {
        diag_emit(DIAG_ERROR, call->as.list.items[0]->span,
                  "expression in call head has type `%s`, which is not callable",
                  type_name(head_expr->type));
        return NULL;
    }

    char tmp_name[32];
    snprintf(tmp_name, sizeof(tmp_name), "__call_head_%u", elab_fresh_id(e));
    const Symbol *tmp_sym = symtab_intern(e->st, strslice(tmp_name, (uint32_t)strlen(tmp_name)));
    Binding *tmp_b = binding_new(e, tmp_sym, head_expr->type, false, false, call->as.list.items[0]->span);

    Expr *source_expr = head_expr;
    while (source_expr && source_expr->kind == EX_ASCRIBE) {
        source_expr = source_expr->as.ascribe_.inner;
    }
    /* closure_fn_binding describes the underlying thunk signature of a closure
     * VALUE (ptr<void>). When the head is itself a function reference (TY_FN),
     * its own TY_FN type already encodes the signature, and conflating it with
     * returns_closure_fn_binding would make `((curry f) x)` dispatch to the
     * inner fn instead of calling f directly. */
    /* any-cannot-recover-a-capturing-closure: the head is a function recovered
     * from an `any`.  Every fn payload in an `any` is fat -- the widen shims a
     * bare one (elab_coerce_to_any) -- so the call has to go through slot 0.
     * Emit reads exactly this flag to choose the fat-dispatch path, the same
     * signal the poly-carrier and cata-carrier heads below use for the same
     * reason: a value whose thunk is chosen at runtime, with no named binding to
     * route through.
     *
     * Without it the head stayed a thin pointer call and the box was jumped into
     * as code -- the segfault `partial-application-widened-to-any-is-a-ptr`
     * turned into a panic, and this turns into an answer. */
    if (head_kind == TY_FN && source_expr && source_expr->kind == EX_ANY_CAST &&
        !head_expr->type.as.fn.cfnptr) {
        tmp_b->type.as.fn.boxed = true;
    } else if (head_kind == TY_PTR_VOID) {
        tmp_b->closure_fn_binding = expr_closure_fn_binding(source_expr);
    } else if (head_kind == TY_FN && source_expr && source_expr->kind == EX_CALL) {
        /* curried-fn-typed-param: the head is the *result of a call* whose
         * static type is itself a function type -- e.g. ((adder 1) 2) where
         * (adder 1) : (fn [int] int).  When the callee genuinely produces a
         * fat closure (its returns_closure_fn_binding is set, as for a defn
         * whose body is a capturing lambda), the runtime value is a heap
         * closure box, not a thin function pointer.  Dispatch the chained
         * application through that closure thunk; otherwise (a call that
         * returns a bare fn reference, e.g. ((pick) 5) -> inc) the head
         * resolves to NULL here and stays a thin pointer call. */
        tmp_b->closure_fn_binding = expr_closure_fn_binding(source_expr);
        /* hkt-cata-function-carrier: the head is the result of a generic call
         * whose DECLARED result is a bare type variable -- e.g.
         * `(cata fn-alg e)` for `(defn cata [B] ... : B)` with B := (fn [int]
         * int).  The TY_FN was recovered from the int64 carrier (Bug 0, #489).
         * Any function value that crosses a generic carrier is a uniform fat
         * box: a bare thin fn passed into a tyvar parameter / parametric ADT
         * field is boxed via EX_FN_TO_FAT, and a closure-returning algebra
         * hands back a heap { thunk, env... } box.  So the chained application
         * MUST dispatch through slot 0, not as a thin pointer (which jumps into
         * the env block -> SIGSEGV).  There is no single named thunk to route
         * through -- the callee body returns `(alg ...)`, whose value is
         * reconstructed from the carrier and (for a match-returning algebra) is
         * a distinct lambda per arm -- so closure_fn_binding stays NULL;
         * instead mark the head temp's fn type `boxed` so emit takes the
         * runtime slot-0 fat-dispatch path (ER2, emit_expr.c). */
        /* MB3 (constrained-hkt-forall-mode-b-plan): the head is a rank-2 POLY
         * CALL whose result is a function -- `(l x)` for `l : forall a. a ->
         * (a -> a)`.  The poly carrier erases the result to the int64 carrier,
         * and the concrete callee (`konst`) hands back a uniform fat closure box
         * (slot 0 = thunk, the box itself = env).  So the chained application
         * `((l x) y)` MUST fat-dispatch through slot 0, not call the box pointer
         * as a thin function pointer (a jump into the env struct -> SIGSEGV).
         * There is no single named thunk (the returned closure is chosen at
         * runtime), so closure_fn_binding stays NULL; mark the head temp `boxed`
         * for the runtime slot-0 fat-dispatch path, mirroring the cata carrier
         * case below. */
        /* hrt-curried-result GRADUATED 2026-07-06: always mark the boxed head. */
        if (!tmp_b->closure_fn_binding &&
            source_expr->as.call_.is_poly_call) {
            tmp_b->type.as.fn.boxed = true;
        }
        if (!tmp_b->closure_fn_binding &&
            source_expr->as.call_.fn_binding &&
            source_expr->as.call_.fn_binding->type.kind == TY_FN &&
            source_expr->as.call_.fn_binding->type.as.fn.result_kind == TY_TYVAR) {
            tmp_b->type.as.fn.boxed = true;
            /* hkt-cata-function-arg: the carrier B is itself a function whose
             * own argument is a function -- B = (fn [(fn [int] int) int] int).
             * The carrier result is fat-dispatched (boxed, above), so every
             * function value flowing into one of its function-typed argument
             * slots must cross the boundary as a uniform fat box too: the
             * dispatcher passes that slot as an opaque int64 and the callee
             * fat-dispatches it (see the matching producer-side fat marking in
             * elab_fn).  Mark each function-typed argument of the carrier fn
             * type `arg_fat`, so the call-site auto-shim boxes a bare/thin fn
             * argument via EX_FN_TO_FAT instead of passing a raw thin pointer
             * the callee then mis-dispatches -> SIGSEGV. */
            if (tmp_b->type.as.fn.arg_full_types) {
                for (uint32_t ai = 0; ai < tmp_b->type.as.fn.arity; ai++) {
                    Type *aft = tmp_b->type.as.fn.arg_full_types[ai];
                    if ((aft && aft->kind == TY_FN && !aft->as.fn.cfnptr) ||
                        (!aft && tmp_b->type.as.fn.arg_kinds[ai] == TY_FN))
                        FN_ARG_SET(tmp_b->type.as.fn, ai, FA_FAT, true);
                }
            }
        }
    } else if (head_kind == TY_FN && source_expr &&
               source_expr->type.kind == TY_PTR_VOID) {
        /* aggregate-return-fat-box-ascription: a :ptr<void> fat-closure box
         * ascribed to a (fn ...) type, then applied --
         * e.g. ((:: ps (fn [float] (Pair float float))) 0.0) where `ps` came
         * from a closure-returning helper.  The `::` re-types the box to TY_FN,
         * so neither the TY_PTR_VOID branch nor the EX_CALL branch above fires;
         * but the runtime value is still a heap closure box and MUST dispatch
         * through slot 0 (read __fn, pass the box as env).  Without this it is
         * called as a thin function pointer -- a jump into the env struct, i.e.
         * a segfault.  Gated on the underlying source actually carrying closure
         * thunk metadata so a raw :ptr<void> callback stays a thin call. */
        tmp_b->closure_fn_binding = expr_closure_fn_binding(source_expr);
    }
    if (source_expr && source_expr->kind == EX_VAR && source_expr->as.var.binding) {
        Binding *source_b = source_expr->as.var.binding;
        if (source_b->is_poly_fn) {
            tmp_b->is_poly_fn = true;
            tmp_b->poly_type = source_b->poly_type;
        }
        /* Propagate "returns a closure" so that chained calls through a let
         * binding (let [g ((curry f) x)] (g y)) see g as callable. */
        if (source_b->returns_closure_fn_binding) {
            tmp_b->returns_closure_fn_binding = source_b->returns_closure_fn_binding;
        }
    }

    Expr *call_expr = elab_call_fn(e, call, tmp_b);
    if (!call_expr) return NULL;
    /* pair-eq-macro-applies-comparator-directly: a lambda applied DIRECTLY has
     * no named callee to carry a convention, but it does not need one -- the
     * argument is right here.  Mark each erased parameter whose argument is a
     * concrete niche option, which IS the payload word. */
    mark_direct_apply_niche_word_params(e, head_expr, call_expr);

    tmp_b->closure_head_init = head_expr;
    LetBinding *let_bs = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
    let_bs->binding = tmp_b;
    let_bs->init = head_expr;

    Expr *let_expr = expr_new(e->arena, EX_LET, call_expr->type, call->span);
    let_expr->as.let_.bindings = let_bs;
    let_expr->as.let_.n = 1;
    let_expr->as.let_.body = call_expr;
    return let_expr;
}

/* ---- general elab ---- */

/* SZ7: static size checking (-Xsized-types).
 * When a call is `(size-assert-eq! a b)` or `(size-assert-le! a b)` and BOTH
 * size arguments reduce to compile-time constants, decide the relation at
 * compile time: a violation is reported with TUR-E0260 (no runtime check is
 * emitted because compilation fails).  When at least one size is not statically
 * known, returns false so the call elaborates normally and the existing runtime
 * assertion guards it -- the checker never silently accepts (SZ7.3). */
static bool sz7_static_size_violation(Elab *e, const Form *call, const Symbol *name) {
    const char *fn = name->name;
    bool is_eq = (strcmp(fn, "size-assert-eq!") == 0);
    bool is_le = (strcmp(fn, "size-assert-le!") == 0);
    if (!is_eq && !is_le) return false;
    if (call->as.list.len != 3) return false;  /* (fn a b) */

    SizeTerm *t0 = size_term_from_form(e->arena, call->as.list.items[1], NULL, NULL);
    SizeTerm *t1 = size_term_from_form(e->arena, call->as.list.items[2], NULL, NULL);
    if (!t0 || !t1) return false;
    int64_t v0, v1;
    if (!size_term_eval(t0, &v0) || !size_term_eval(t1, &v1)) return false; /* runtime fallback */

    if (is_eq && v0 != v1) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0260_SIZED_TYPE_MISMATCH,
            "sized type mismatch (TUR-E0260): size %lld is not %lld",
            (long long)v0, (long long)v1);
        return true;
    }
    if (is_le && v0 > v1) {
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0260_SIZED_TYPE_MISMATCH,
            "sized type mismatch (TUR-E0260): size %lld exceeds upper bound %lld",
            (long long)v0, (long long)v1);
        return true;
    }
    return false;
}

/* SZ8: infer the type-level size index of a sized-GADT constructor application.
 * The index is computed by substituting each operand's already-inferred index
 * into the constructor's declared return-type index template. For example,
 * given `SVCons : int -> (SizedVec n) -> (SizedVec (Add (Static 1) n))`, a call
 * `(SVCons 7 v)` where `v` has inferred index `t` yields `(Add (Static 1) t)`.
 * SVNil's template `(Static 0)` is already closed, seeding the recursion.
 *
 * Returns the inferred SizeTerm (arena-allocated), or NULL when the GADT is not
 * size-indexed or an operand index is unknown (the size stays polymorphic).
 * Inference is purely additive metadata -- erased in codegen. */
static SizeTerm *sz8_infer_ctor_size_index(Elab *e, const CtorDef *ctor,
                                           Expr *call_expr) {
    if (!ctor || !ctor->adt || !ctor->adt->is_gadt)
        return NULL;
    const AdtDef *adt = ctor->adt;
    const Form *rt = ctor->result_type_form;
    if (!rt || rt->tag != F_LIST || rt->as.list.len < 2) return NULL;

    /* Find the size-index parameter position: the first return-type argument
     * that parses as a Size expression. */
    int size_pos = -1;
    SizeTerm *template = NULL;
    for (uint32_t i = 1; i < rt->as.list.len; i++) {
        SizeTerm *st = size_term_from_form(e->arena, rt->as.list.items[i], NULL, NULL);
        if (st) { size_pos = (int)i - 1; template = st; break; }
    }
    if (size_pos < 0 || !template) return NULL;

    /* For each field that is itself a value of the SAME sized GADT, map the
     * field's declared index variable to the argument's inferred index. */
    SizeTerm *result = template;
    uint32_t n_call_args = call_expr->as.call_.n_args;
    for (uint32_t fi = 0; fi < ctor->n_fields && fi < n_call_args; fi++) {
        const Form *ff = ctor->field_forms ? ctor->field_forms[fi] : NULL;
        if (!ff || ff->tag != F_LIST || ff->as.list.len < 2) continue;
        const Form *fhd = ff->as.list.items[0];
        if (fhd->tag != F_SYM || strcmp(fhd->as.sym->name, adt->name) != 0) continue;
        if ((uint32_t)(size_pos + 1) >= ff->as.list.len) continue;
        const Form *fidx = ff->as.list.items[size_pos + 1];
        if (fidx->tag != F_SYM) continue;            /* only a bare index var threads */
        const char *fvar = fidx->as.sym->name;
        Expr *arg = call_expr->as.call_.args[fi];
        SizeTerm *arg_idx = (arg && arg->kind == EX_CALL)
                          ? arg->as.call_.size_index : NULL;
        if (!arg_idx) return NULL;                   /* operand unknown -> not inferable */
        result = size_term_subst(e->arena, result, fvar, arg_idx);
    }
    return result;
}

/* SZ8: true when `ctor` is a size-indexed GADT constructor (its return type
 * carries a Size expression in some argument position). */
static bool sz8_ctor_is_sized(Elab *e, const CtorDef *ctor) {
    if (!ctor || !ctor->adt || !ctor->adt->is_gadt)
        return false;
    const Form *rt = ctor->result_type_form;
    if (!rt || rt->tag != F_LIST || rt->as.list.len < 2) return false;
    for (uint32_t i = 1; i < rt->as.list.len; i++)
        if (size_term_from_form(e->arena, rt->as.list.items[i], NULL, NULL))
            return true;
    return false;
}

/* SZ8 projection-size recovery (sz8-projection-size-recovery-gap): recover the
 * declared type-annotation Form describing expr `x`'s static type, so cross-
 * parameter size unification can re-extract a size index from arguments that
 * are not direct calls.  Handles:
 *   - EX_VAR        -> the binding's retained `decl_type_form`
 *   - EX_CALL       -> the callee's declared return-type Form
 *   - EX_GET_FIELD  -> the receiver's recovered type Form, projected to the
 *                      struct type argument the field's (bare) type variable
 *                      selects (e.g. `.fst` of `(Pair2 (Dense (Static 3) A) ..)`
 *                      yields `(Dense (Static 3) A)`).
 *   - EX_ASCRIBE    -> the ascribed type Form when it is a compound
 *                      `(Head ...)` (declared-size-index-never-checked-against-
 *                      value, half B: `(:: e (SizedBuf (Static 4)))` pins the
 *                      index the same way a declared return type does); a
 *                      bare-keyword ascription carries no index and falls
 *                      through to the inner expression.
 * Returns NULL when no Form is recoverable (the size stays polymorphic). */
const Form *sz_recover_type_form(Elab *e, const Expr *x) {
    if (!x) return NULL;
    switch (x->kind) {
        case EX_VAR:
            return x->as.var.binding ? x->as.var.binding->decl_type_form : NULL;
        case EX_ASCRIBE: {
            const Form *tf = x->as.ascribe_.type_form;
            if (tf && tf->tag == F_LIST && tf->as.list.len >= 2) return tf;
            return sz_recover_type_form(e, x->as.ascribe_.inner);
        }
        case EX_CALL: {
            /* gadt-length-index-not-enforced: a GADT constructor application
             * knows its own index, which the constructor's declared result
             * (the open template below) does not. */
            if (x->as.call_.gadt_form) return x->as.call_.gadt_form;
            const Binding *callee = x->as.call_.fn_binding;
            if (!callee) return NULL;
            const Type *cft = &callee->type;
            if (callee->closure_fn_binding) cft = &callee->closure_fn_binding->type;
            if (cft && cft->kind == TY_FN && cft->as.fn.result_type_form)
                return cft->as.fn.result_type_form;
            return NULL;
        }
        case EX_GET_FIELD: {
            uint32_t fi = x->as.get_field_.field_idx;
            /* Recover the field's declared type + the owner's type-params from
             * either a struct receiver OR a lowered record-ADT receiver (under
             * defstruct-as-defadt the StructDef* `def` is NULL and the field
             * access carries `adt_def`/`adt_ctor` instead).  Both back the same
             * size-index projection. */
            const Type *field_full = NULL;
            const char **type_params = NULL;
            uint8_t n_type_params = 0;
            if (x->as.get_field_.adt_def && x->as.get_field_.adt_ctor &&
                       fi < x->as.get_field_.adt_ctor->n_fields) {
                const struct AdtDef *ad = x->as.get_field_.adt_def;
                field_full = x->as.get_field_.adt_ctor->fields[fi].full_type;
                type_params = ad->type_params;
                n_type_params = ad->n_type_params;
            } else {
                return NULL;
            }
            /* Only a field whose declared type is a bare type variable threads a
             * recoverable index: map that variable to its type-parameter
             * position, then index into the receiver Form. */
            if (!field_full || field_full->kind != TY_TYVAR ||
                !field_full->as.tyvar_.name)
                return NULL;
            uint8_t pi = 0;
            bool found = false;
            for (uint8_t i = 0; i < n_type_params; i++) {
                if (type_params[i] &&
                    strcmp(type_params[i], field_full->as.tyvar_.name) == 0) {
                    pi = i; found = true; break;
                }
            }
            if (!found) return NULL;
            const Form *recv = sz_recover_type_form(e, x->as.get_field_.struct_expr);
            if (!recv || recv->tag != F_LIST) return NULL;
            /* recv = (StructName arg0 arg1 ...); the type arg is at items[1+pi]. */
            if ((uint32_t)(1 + pi) >= recv->as.list.len) return NULL;
            return recv->as.list.items[1 + pi];
        }
        default:
            return NULL;
    }
}

/* declared-size-index-never-checked-against-value: reconcile a WRITTEN size
 * claim with the value it describes.  `claim` is a `(Head idx ...)` type-app
 * Form (a defn's declared return type, or a `::` ascription); `x` is the
 * expression the claim is made about.  Walks every index position of the
 * claim, recovers the expression's static index at the same position (its
 * declared/ascribed type Form, or the GADT-inferred ctor index), and reports
 * the first position where BOTH fold to constants and disagree.  A position
 * where either side is open (a size variable, an un-inferable operand) stays
 * polymorphic and is skipped -- ascribing a runtime-sized value is the
 * legitimate case and must keep working.  This is the same fold-and-compare
 * sz_cross_param_unify does in its closed-template arm, applied at the seam
 * where the claim is MADE rather than where it is consumed. */
bool sz_claim_disagrees(Elab *e, const Form *claim, const Expr *x,
                        int64_t *claimed, int64_t *actual) {
    if (!claim || claim->tag != F_LIST || claim->as.list.len < 2 || !x)
        return false;
    const Form *xf = sz_recover_type_form(e, x);
    const SizeTerm *gadt_idx = (x->kind == EX_CALL) ? x->as.call_.size_index
                                                    : NULL;
    if (!xf && !gadt_idx) return false;
    for (uint32_t k = 1; k < claim->as.list.len; k++) {
        SizeTerm *ct = size_term_from_form(e->arena, claim->as.list.items[k],
                                           NULL, NULL);
        if (!ct) continue;                       /* not a size position */
        int64_t cv;
        if (!size_term_eval(ct, &cv)) continue;  /* open claim -> polymorphic */
        const SizeTerm *xt = NULL;
        if (xf && xf->tag == F_LIST && k < xf->as.list.len)
            xt = size_term_from_form(e->arena, xf->as.list.items[k], NULL, NULL);
        int64_t xv;
        if (!(xt && size_term_eval(xt, &xv))) {
            if (!(gadt_idx && size_term_eval(gadt_idx, &xv))) continue;
        }
        if (xv != cv) {
            *claimed = cv;
            *actual  = xv;
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * gadt-length-index-not-enforced: the phantom INDEX of a GADT value, carried
 * as a type Form beside the value's (bare) type.
 *
 * A GADT constructor application is typed as the bare ADT -- `(VNil)` and
 * `(VCons 7 (VNil))` are both `Vec` -- because the checker treats a bare `Vec`
 * and an application `(Vec Zero)` as different types, and every bare-annotated
 * GADT function (stdlib/gadt-vec.tur included) would otherwise stop accepting
 * the values its own constructors build.  So the index is not put in the TYPE.
 * It rides the same channel the sized-GADT (SZ8) indices already do: a Form,
 * recovered from an expression by sz_recover_type_form -- a constructor
 * application's inferred index (call_.gadt_form, below), a let binding's
 * decl_type_form, an ascription, a callee's declared result -- and checked
 * against a declared template where a claim is made: a call argument, an
 * ascription, a function's declared return.
 *
 * The matcher only ever REJECTS what is provably wrong.  Index symbols that
 * name a type in scope (`Zero`, `Succ`, `int`) are constants; any other symbol
 * -- a type variable, or `?`, how an index the constructor could not infer is
 * written -- matches anything.  Two constants disagree only when they resolve
 * to different types, so `(Vec (Succ n))` against `(Vec Zero)` is a mismatch
 * and `(Vec (Succ n))` against `(Vec m)` is not.  A size term (Static / Add /
 * Mul) is SZ8's business and is never judged here.
 * ========================================================================== */

typedef struct { const char *name; const Form *val; } GxSub;
#define GX_MAX_SUB 16

/* A spaced let annotation `[v : (Vec Zero) ...]` reaches us wrapped in the
 * reader's one-item F_TYPE_ANN (and so does the binding's decl_type_form). */
static const Form *gx_peel(const Form *f) {
    while (f && f->tag == F_TYPE_ANN && f->as.list.len == 1)
        f = f->as.list.items[0];
    return f;
}

/* 0 = not a constant (a variable / `?` / something unclassified), 1 = a
 * primitive type keyword (`*prim` set), 2 = a named ADT/opaque (`*def` set). */
static int gx_const(Elab *e, const Form *f, TypeKind *prim, const AdtDef **def) {
    if (!f || f->tag != F_SYM || !f->as.sym) return 0;
    const char *n = f->as.sym->name;
    static const struct { const char *name; TypeKind kind; } prims[] = {
        { "int", TY_INT }, { "bool", TY_BOOL }, { "float", TY_FLOAT },
        { "cstr", TY_CSTR }, { "int8", TY_INT8 }, { "int16", TY_INT16 },
        { "int32", TY_INT32 }, { "int64", TY_INT64 }, { "uint8", TY_UINT8 },
        { "uint16", TY_UINT16 }, { "uint32", TY_UINT32 },
        { "uint64", TY_UINT64 }, { "float32", TY_FLOAT32 },
        { "float64", TY_FLOAT64 },
    };
    for (size_t i = 0; i < sizeof prims / sizeof prims[0]; i++)
        if (strcmp(prims[i].name, n) == 0) { *prim = prims[i].kind; return 1; }
    Binding *tb = scope_lookup(e->scope, f->as.sym);
    if (!tb) tb = scope_lookup(&e->global, f->as.sym);
    /* Only a binding whose type IS the named ADT counts; an alias to an
     * application, a value, anything else is left unclassified. */
    if (tb && tb->type.kind == TY_ADT && tb->type.as.adt_.def) {
        *def = tb->type.as.adt_.def;
        return 2;
    }
    return 0;
}

static bool gx_is_size_head(const Form *hd) {
    if (!hd || hd->tag != F_SYM) return false;
    const char *op = hd->as.sym->name;
    return strcmp(op, "Static") == 0 || strcmp(op, "Add") == 0 ||
           strcmp(op, "Mul") == 0;
}

/* Could `a` and `b` be the same index?  False only when provably not. */
static bool gx_compatible(Elab *e, const Form *a, const Form *b) {
    a = gx_peel(a); b = gx_peel(b);
    if (!a || !b) return true;
    TypeKind pa = TY_UNKNOWN, pb = TY_UNKNOWN;
    const AdtDef *da = NULL, *db = NULL;
    if (a->tag == F_SYM && b->tag == F_SYM) {
        int ka = gx_const(e, a, &pa, &da), kb = gx_const(e, b, &pb, &db);
        if (!ka || !kb) return true;
        if (ka != kb) return false;
        return ka == 1 ? pa == pb : da == db;
    }
    if (a->tag == F_SYM || b->tag == F_SYM) {
        const Form *sym = a->tag == F_SYM ? a : b;
        const Form *lst = a->tag == F_SYM ? b : a;
        if (lst->tag != F_LIST || lst->as.list.len < 1) return true;
        int ks = gx_const(e, sym, &pa, &da);
        if (!ks) return true;
        const Form *hd = lst->as.list.items[0];
        if (gx_is_size_head(hd)) return true;
        int kh = gx_const(e, hd, &pb, &db);
        if (kh != 2) return true;
        return ks == 2 && da == db;          /* nullary name vs an application */
    }
    if (a->tag != F_LIST || b->tag != F_LIST) return true;
    if (a->as.list.len < 1 || b->as.list.len < 1) return true;
    const Form *ha = a->as.list.items[0], *hb = b->as.list.items[0];
    if (gx_is_size_head(ha) || gx_is_size_head(hb)) return true;
    int ka = gx_const(e, ha, &pa, &da), kb = gx_const(e, hb, &pb, &db);
    if (ka != 2 || kb != 2) return true;
    if (da != db) return false;
    if (a->as.list.len != b->as.list.len) return true;
    for (uint32_t i = 1; i < a->as.list.len; i++)
        if (!gx_compatible(e, a->as.list.items[i], b->as.list.items[i]))
            return false;
    return true;
}

/* Match the template `pat` against the value index `val`, binding `pat`'s
 * variables in `subs` (shared across one call's parameters, so a variable
 * two parameters name must agree).  False only on a provable mismatch. */
static bool gx_unify(Elab *e, const Form *pat, const Form *val,
                     GxSub *subs, int *n_subs) {
    pat = gx_peel(pat); val = gx_peel(val);
    if (!pat || !val) return true;
    TypeKind pp = TY_UNKNOWN;
    const AdtDef *pd = NULL;
    if (pat->tag == F_SYM) {
        if (gx_const(e, pat, &pp, &pd)) return gx_compatible(e, pat, val);
        const char *vn = pat->as.sym->name;
        TypeKind vp = TY_UNKNOWN;
        const AdtDef *vd = NULL;
        /* A value side that says nothing binds nothing. */
        if (val->tag == F_SYM && !gx_const(e, val, &vp, &vd)) return true;
        for (int i = 0; i < *n_subs; i++)
            if (strcmp(subs[i].name, vn) == 0)
                return gx_compatible(e, subs[i].val, val);
        if (*n_subs < GX_MAX_SUB) {
            subs[*n_subs].name = vn;
            subs[*n_subs].val = val;
            (*n_subs)++;
        }
        return true;
    }
    if (pat->tag != F_LIST || pat->as.list.len < 1) return true;
    if (gx_is_size_head(pat->as.list.items[0])) return true;
    if (val->tag == F_SYM) return gx_compatible(e, pat, val);
    if (val->tag != F_LIST || val->as.list.len < 1) return true;
    const Form *hp = pat->as.list.items[0], *hv = val->as.list.items[0];
    if (gx_is_size_head(hv)) return true;
    TypeKind vp = TY_UNKNOWN;
    const AdtDef *vd = NULL;
    int kp = gx_const(e, hp, &pp, &pd), kv = gx_const(e, hv, &vp, &vd);
    if (kp == 2 && kv == 2 && pd != vd) return false;
    if (kp != 2 || kv != 2) return true;
    if (pat->as.list.len != val->as.list.len) return true;
    for (uint32_t i = 1; i < pat->as.list.len; i++)
        if (!gx_unify(e, pat->as.list.items[i], val->as.list.items[i], subs, n_subs))
            return false;
    return true;
}

/* `f` with its variables replaced from `subs`; an unbound variable becomes
 * `?`, so a partly inferred index still says what it knows. */
static const Form *gx_subst(Elab *e, const Form *f, const GxSub *subs, int n_subs,
                            const Form *qmark) {
    if (!f) return NULL;
    if (f->tag == F_SYM) {
        TypeKind p = TY_UNKNOWN;
        const AdtDef *d = NULL;
        if (gx_const(e, f, &p, &d)) return f;
        for (int i = 0; i < n_subs; i++)
            if (strcmp(subs[i].name, f->as.sym->name) == 0) return subs[i].val;
        return qmark;
    }
    if (f->tag != F_LIST || f->as.list.len < 1) return f;
    if (gx_is_size_head(f->as.list.items[0])) return f;
    Form **items = (Form **)arena_alloc(e->arena, f->as.list.len * sizeof(Form *));
    items[0] = f->as.list.items[0];
    for (uint32_t i = 1; i < f->as.list.len; i++)
        items[i] = (Form *)gx_subst(e, f->as.list.items[i], subs, n_subs, qmark);
    return form_list(e->arena, f->span, items, f->as.list.len);
}

static const AdtDef *gx_gadt_head(Elab *e, const Form *f) {
    f = gx_peel(f);
    if (!f || f->tag != F_LIST || f->as.list.len < 2) return NULL;
    TypeKind p = TY_UNKNOWN;
    const AdtDef *d = NULL;
    if (gx_const(e, f->as.list.items[0], &p, &d) != 2 || !d || !d->is_gadt)
        return NULL;
    return d;
}

static char *gx_form_str(const Form *f) {
    Buf b; buf_init(&b);
    form_print(&b, f);
    buf_putc(&b, '\0');
    char *s = strdup(b.data ? b.data : "?");
    buf_free(&b);
    return s;
}

/* The index of a GADT constructor application: the constructor's declared
 * result with its variables instantiated from the fields' arguments.
 * `(VCons 7 (VNil))` is `(Vec (Succ Zero))`; with an argument whose index is
 * unknown it is `(Vec (Succ ?))`.  A field whose argument's index contradicts
 * another field sharing its variable is a type error, reported here.
 * NULL for a non-GADT. */
static const Form *gadt_infer_ctor_form(Elab *e, const CtorDef *ctor,
                                        const Expr *call_expr, const Form *call,
                                        bool *mismatch) {
    if (mismatch) *mismatch = false;
    if (!ctor || !ctor->adt || !ctor->adt->is_gadt) return NULL;
    /* A size term (SZ8's `(Static 0)`, `(Add (Static 1) n)`) is left exactly
     * as declared by gx_subst and matches anything in gx_unify, so a sized
     * GADT's form carries the same size template it always did. */
    const Form *rt = ctor->result_type_form;
    if (!rt || rt->tag != F_LIST || rt->as.list.len < 2) return NULL;
    GxSub subs[GX_MAX_SUB];
    int n_subs = 0;
    uint32_t n_args = call_expr ? call_expr->as.call_.n_args : 0;
    for (uint32_t fi = 0; fi < ctor->n_fields && fi < n_args; fi++) {
        const Form *ff = ctor->field_forms ? ctor->field_forms[fi] : NULL;
        if (!ff || ff->tag != F_LIST) continue;
        const Form *af = gx_peel(sz_recover_type_form(e, call_expr->as.call_.args[fi]));
        if (!af || af->tag != F_LIST) continue;
        if (!gx_unify(e, ff, af, subs, &n_subs)) {
            char *fs = gx_form_str(gx_subst(e, ff, subs, n_subs,
                                            form_sym(e->arena, ff->span,
                                                     symtab_intern(e->st, strslice("?", 1)))));
            char *as = gx_form_str(af);
            Span sp = (call && call->tag == F_LIST && fi + 1 < call->as.list.len)
                      ? call->as.list.items[fi + 1]->span : call_expr->span;
            diag_emit_with_code(DIAG_ERROR, sp, TUR_E0001_TYPE_MISMATCH,
                                "constructor '%s' arg %u: expected %s, got %s",
                                ctor->name, fi + 1, fs, as);
            free(fs); free(as);
            if (mismatch) *mismatch = true;
            return NULL;
        }
    }
    const Form *qmark = form_sym(e->arena, rt->span,
                                 symtab_intern(e->st, strslice("?", 1)));
    return gx_subst(e, rt, subs, n_subs, qmark);
}

/* Does `f` name only constants -- an index known all the way down? */
static bool gx_closed(Elab *e, const Form *f) {
    f = gx_peel(f);
    if (!f) return false;
    if (f->tag == F_SYM) {
        TypeKind p = TY_UNKNOWN;
        const AdtDef *d = NULL;
        return gx_const(e, f, &p, &d) != 0;
    }
    if (f->tag != F_LIST || f->as.list.len < 1) return false;
    if (gx_is_size_head(f->as.list.items[0])) return false;
    for (uint32_t i = 0; i < f->as.list.len; i++)
        if (!gx_closed(e, f->as.list.items[i])) return false;
    return true;
}

/* A bare GADT value `x` whose index is known all the way down, retyped at
 * that index through an ascription -- the node `(:: x (Vec (Succ Zero)))`
 * elaborates to.  `x` itself when the index is not known. */
Expr *gadt_refine_to_index(Elab *e, Expr *x) {
    if (!x || x->type.kind != TY_ADT || !x->type.as.adt_.def ||
        !x->type.as.adt_.def->is_gadt)
        return x;
    const Form *xf = gx_peel(sz_recover_type_form(e, x));
    if (gx_gadt_head(e, xf) != x->type.as.adt_.def || !gx_closed(e, xf))
        return x;
    Type *t = fn_type_from_form(e, xf, NULL, NULL, 0);
    if (!t || t->kind != TY_APP || type_adt_app_def(t) != x->type.as.adt_.def)
        return x;
    Expr *asc = expr_new(e->arena, EX_ASCRIBE, *t, x->span);
    asc->as.ascribe_.inner = x;
    asc->as.ascribe_.type_form = xf;
    return asc;
}

/* A call's GADT-indexed parameters against what each argument's index is
 * known to be.  One substitution for the whole call, so `(zip-with [n] [a :
 * (Vec n) b : (Vec n)])` rejects two different lengths.  Returns true when it
 * reported a mismatch. */
static bool gadt_index_check_call(Elab *e, const Form *call, const Type *fn_type,
                                  Binding *fn_binding, Expr **args,
                                  uint32_t n_args) {
    if (!fn_type || fn_type->kind != TY_FN || !fn_type->as.fn.param_type_forms)
        return false;
    if (fn_binding && fn_binding->closure_fn_binding) return false;
    GxSub subs[GX_MAX_SUB];
    int n_subs = 0;
    uint32_t arity = fn_type->as.fn.arity;
    for (uint32_t i = 0; i < n_args && i < arity; i++) {
        const Form *pf = gx_peel(fn_type->as.fn.param_type_forms[i]);
        const AdtDef *pd = gx_gadt_head(e, pf);
        if (!pd) continue;
        const Form *af = gx_peel(sz_recover_type_form(e, args[i]));
        if (gx_gadt_head(e, af) != pd) continue;
        if (!gx_unify(e, pf, af, subs, &n_subs)) {
            char *ps = gx_form_str(pf);
            char *as = gx_form_str(af);
            Span sp = (call && call->tag == F_LIST && i + 1 < call->as.list.len)
                      ? call->as.list.items[i + 1]->span : args[i]->span;
            diag_emit_with_code(DIAG_ERROR, sp, TUR_E0001_TYPE_MISMATCH,
                                "function '%s' arg %u: expected %s, got %s",
                                (fn_binding && fn_binding->name)
                                    ? fn_binding->name->name : "?",
                                i + 1, ps, as);
            free(ps); free(as);
            return true;
        }
    }
    return false;
}

/* A CLAIM -- an ascription, a declared return, an annotated let -- that a
 * GADT value has index `claim`, against what `x`'s index is known to be.
 * True (and `*got` set, arena-owned) when the claim is provably false. */
bool gadt_claim_disagrees(Elab *e, const Form *claim, const Expr *x,
                          const Form **got) {
    claim = gx_peel(claim);
    const AdtDef *cd = gx_gadt_head(e, claim);
    if (!cd || !x) return false;
    /* An annotated let (and a return the elaborator pinned to its declared
     * type) wraps the value in an ascription carrying the very claim being
     * checked; what it claims ABOUT is underneath. */
    while (x && x->kind == EX_ASCRIBE &&
           (gx_peel(x->as.ascribe_.type_form) == claim ||
            x->as.ascribe_.type_form == NULL))
        x = x->as.ascribe_.inner;
    const Form *xf = gx_peel(sz_recover_type_form(e, x));
    if (gx_gadt_head(e, xf) != cd) return false;
    if (xf == claim) return false;
    GxSub subs[GX_MAX_SUB];
    int n_subs = 0;
    if (gx_unify(e, claim, xf, subs, &n_subs)) return false;
    if (got) *got = xf;
    return true;
}

/* (sz_first_size_term was retired by sized-types-cross-param-multi-index:
 * sz_cross_param_unify now walks every index position directly rather than
 * collapsing a multi-index opaque to its first size term.) */
#if 0
static SizeTerm *sz_first_size_term(Elab *e, const Form *tform) {
    if (!tform || tform->tag != F_LIST) return NULL;
    for (uint32_t k = 1; k < tform->as.list.len; k++) {
        SizeTerm *st = size_term_from_form(e->arena, tform->as.list.items[k],
                                           NULL, NULL);
        if (st) return st;
    }
    return NULL;
}
#endif

/* sized-types-cross-param-unification: a function signature that names the
 * same size variable in two or more parameters (e.g. both `xs : (SizedVec n)`
 * and `ys : (SizedVec n)`) shares scope on `n`; the elaborator must reject
 * callers whose corresponding arguments carry statically-known and unequal
 * size indices.  Walks the callee's retained per-parameter type-annotation
 * Forms paired with each elaborated argument's inferred `size_index`,
 * maintains a per-call substitution table for bare-symbol size variables,
 * and emits TUR-E0260 on any disagreement.  A parameter whose template is
 * closed (e.g. `(SizedVec (Static 3))`) is compared by folded value; a
 * parameter whose template is a complex open expression containing vars,
 * or whose arg has no inferred index, is skipped (stays polymorphic).
 * Returns true on a static mismatch (the caller should treat the call as
 * already diagnosed and bail). */
static bool sz_cross_param_unify(Elab *e, const Form *call,
                                 const Type *fn_type, Binding *fn_binding,
                                 Expr **args, uint32_t n_args) {
    if (!fn_type || fn_type->kind != TY_FN) return false;
    if (!fn_type->as.fn.param_type_forms) return false;
    /* Skip closure-thunk hidden-env shift; the param_type_forms array was
     * built from the source signature without the env slot. */
    if (fn_binding && fn_binding->closure_fn_binding) return false;
    /* Per-call substitution table: bare-symbol size variable -> bound term. */
    struct { const char *name; const SizeTerm *bound; uint32_t arg_idx; } subst[8];
    uint8_t n_subst = 0;
    uint32_t arity = fn_type->as.fn.arity;
    for (uint32_t i = 0; i < n_args && i < arity; i++) {
        const Form *pf = fn_type->as.fn.param_type_forms[i];
        if (!pf || pf->tag != F_LIST || pf->as.list.len < 2) continue;
        Expr *a = args[i];
        /* Recover the argument's declared type Form once; its index positions
         * line up one-for-one with the parameter template's positions (same
         * opaque/GADT head), so a multi-index opaque (e.g. `(Mat m n)`) can be
         * unified position-wise.  This covers direct calls whose callee
         * declares a Size-carrying return type (`mk-dense-2 : (Dense (Static 2)
         * A)`), plain variables that flow a sized value (`let [a (mk-2)] .. a`),
         * and struct field projections. */
        const Form *af = sz_recover_type_form(e, a);
        /* GADT-inferred single index (set by sz8_infer_ctor_size_index for
         * sized-GADT constructor applications, whose recovered return Form is
         * an OPEN template like `(SizedVec (Add (Static 1) n))`). */
        const SizeTerm *gadt_idx = (a && a->kind == EX_CALL)
                                   ? a->as.call_.size_index : NULL;
        /* Walk EVERY index position of the parameter template, not just the
         * first.  The single-index case is the degenerate form (one size
         * position); a multi-index opaque contributes one size binding per
         * position so a variable shared across parameters is contradicted at
         * whichever slot it occupies. */
        for (uint32_t k = 1; k < pf->as.list.len; k++) {
            const Form *tmpl_form = pf->as.list.items[k];
            SizeTerm *tmpl_probe = size_term_from_form(e->arena, tmpl_form,
                                                       NULL, NULL);
            if (!tmpl_probe) continue;  /* non-size arg (e.g. element type param) */
            /* Arg's size term at the SAME position.  Prefer the recovered type
             * Form (covers opaque multi-index + concrete return forms); fall
             * back to the GADT-inferred index when the recovered term is not a
             * folded constant (i.e. an open ctor-return template). */
            const SizeTerm *arg_idx = NULL;
            if (af && af->tag == F_LIST && k < af->as.list.len)
                arg_idx = size_term_from_form(e->arena, af->as.list.items[k],
                                              NULL, NULL);
            if (gadt_idx) {
                int64_t tmp;
                if (!(arg_idx && size_term_eval(arg_idx, &tmp)))
                    arg_idx = gadt_idx;
            }
            if (!arg_idx) continue;  /* un-inferable -> stays polymorphic */
            /* Case 1: template is a bare size variable -- bind it in subst, or
             * require equality with the prior binding. */
            if (tmpl_form->tag == F_SYM) {
                const char *vname = tmpl_form->as.sym->name;
                bool found = false;
                for (uint8_t s = 0; s < n_subst; s++) {
                    if (strcmp(subst[s].name, vname) != 0) continue;
                    found = true;
                    int64_t va, vb;
                    bool ca = size_term_eval(subst[s].bound, &va);
                    bool cb = size_term_eval(arg_idx, &vb);
                    if (ca && cb && va != vb) {
                        diag_emit_with_code(DIAG_ERROR,
                            call->as.list.items[1 + i]->span,
                            TUR_E0260_SIZED_TYPE_MISMATCH,
                            "sized type mismatch (TUR-E0260): function '%s' "
                            "shares size variable '%s' across parameters, "
                            "but argument %u has size %lld while argument %u "
                            "has size %lld",
                            (fn_binding && fn_binding->name)
                                ? fn_binding->name->name : "?",
                            vname,
                            (unsigned)(subst[s].arg_idx + 1), (long long)va,
                            (unsigned)(i + 1), (long long)vb);
                        return true;
                    }
                    break;
                }
                if (!found && n_subst < 8) {
                    subst[n_subst].name    = vname;
                    subst[n_subst].bound   = arg_idx;
                    subst[n_subst].arg_idx = i;
                    n_subst++;
                }
                continue;
            }
            /* Case 2: template is a closed size expression (no free vars) --
             * compare by folded value against the arg's inferred index. */
            int64_t tv, av;
            if (size_term_eval(tmpl_probe, &tv) &&
                size_term_eval(arg_idx, &av) && tv != av) {
                diag_emit_with_code(DIAG_ERROR,
                    call->as.list.items[1 + i]->span,
                    TUR_E0260_SIZED_TYPE_MISMATCH,
                    "sized type mismatch (TUR-E0260): argument %u of '%s' has "
                    "size %lld but parameter declares size %lld",
                    (unsigned)(i + 1),
                    (fn_binding && fn_binding->name)
                        ? fn_binding->name->name : "?",
                    (long long)av, (long long)tv);
                return true;
            }
            /* Open templates with internal vars: deferred (matches the SZ8
             * single-parameter polymorphism baseline). */
        }
    }
    return false;
}

/* SZ8: --dump-sizes -- emit one line per size-indexed constructor application.
 * A folded constant prints as the number; an open term prints symbolically;
 * an un-inferable index (an operand whose size is unknown) prints as `?`. */
static void sz8_dump_ctor_size(Elab *e, const CtorDef *ctor,
                               const SizeTerm *inferred) {
    if (!g_dump_sizes || !sz8_ctor_is_sized(e, ctor)) return;
    char sbuf[128];
    int64_t k;
    if (inferred && size_term_eval(inferred, &k))
        fprintf(stderr, "size: %s : (%s %lld)\n",
                ctor->name, ctor->adt->name, (long long)k);
    else if (inferred)
        fprintf(stderr, "size: %s : (%s %s)\n", ctor->name, ctor->adt->name,
                size_term_to_string(inferred, sbuf, sizeof(sbuf)));
    else
        fprintf(stderr, "size: %s : (%s ?)\n", ctor->name, ctor->adt->name);
}

/* Phase GHE1: does `name` name a method of some registered typeclass?
 * Used to route a bare-name method call (hash x) / (eq? a b) to the same
 * argument-type dispatch the dotted (.method ...) form performs, but only
 * when no ordinary binding (user defn or local shadow) claims the name. */
static bool elab_name_is_typeclass_method(Elab *e, const Symbol *name) {
    if (!name) return false;
    for (TypeClass *c = e->typeclass_env.typeclasses; c != NULL; c = c->next) {
        for (uint8_t mi = 0; mi < c->n_methods; mi++) {
            const Symbol *mn = c->methods[mi].name;
            if (mn && mn->len == name->len &&
                memcmp(mn->name, name->name, name->len) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* N0 (numeric-tower-rational-complex-plan §3): map an arithmetic operator
 * symbol onto the `Num` method that implements it.  `n_args` picks between the
 * binary reading of `-` (sub) and the unary one (neg); the other operators are
 * binary/variadic only.  Returns NULL when `name` is not an arithmetic
 * operator, so every other builtin miss keeps its existing diagnostic. */
static const char *num_method_for_operator(const Symbol *name, uint32_t n_args) {
    if (!name || name->len != 1) return NULL;
    switch (name->name[0]) {
        case '+': return n_args >= 2 ? "add" : NULL;
        case '-': return n_args >= 2 ? "sub" : (n_args == 1 ? "neg" : NULL);
        case '*': return n_args >= 2 ? "mul" : NULL;
        case '/': return n_args >= 2 ? "div" : NULL;
        default:  return NULL;
    }
}

/* N0: true when some registered `Num` instance dispatches on exactly `t`.
 * Matched by full structural equality rather than by TypeKind, so a `Num`
 * instance for one by-value product never captures an unrelated one. */
static bool elab_num_instance_matches(Elab *e, const TypeClass *num,
                                      const Type *t) {
    if (!num || !t || t->kind == TY_UNKNOWN) return false;
    for (TypeClassInstance *inst = e->typeclass_env.instances; inst;
         inst = inst->next) {
        if (inst->typeclass != num || inst->n_type_args != 1) continue;
        if (type_eq(inst->type_args[0], *t)) return true;
    }
    return false;
}

/* N0: fall back from a builtin-operator miss to `Num` typeclass dispatch.
 *
 * `+`/`-`/`*`/`/` are BuiltinSpec rows keyed by TypeKind that emit a C infix
 * operator (src/compiler/builtins.c), a shape that cannot express arithmetic
 * over a struct and has no way to name a specific ADT anyway.  Rather than
 * bolting Rational/Complex rows onto that table, a miss re-reads the call as
 * the corresponding `Num` method, which gives operator overloading to every
 * user numeric type.  Primitive arithmetic is untouched: the builtin row still
 * wins whenever it matches, so there is no codegen drift on existing programs
 * and no dictionary in the hot path.
 *
 * Variadic calls left-fold into nested binary method calls, matching
 * BS_VARIADIC_FOLD: `(+ a b c)` becomes `(.add (.add a b) c)`.
 *
 * Returns NULL (having consumed nothing) when the call is not arithmetic, no
 * `Num` class is in scope, or no instance dispatches on the receiver -- the
 * caller then proceeds to its usual operator-lookup-failed diagnostic. */
static Expr *elab_try_num_operator_dispatch(Elab *e, const Form *call,
                                            const Form *head,
                                            const Symbol *name,
                                            const Type *first_t,
                                            uint32_t n_args) {
    const char *method = num_method_for_operator(name, n_args);
    if (!method) return NULL;

    TypeClass *num = typeclass_env_lookup_typeclass(
        &e->typeclass_env, symtab_intern(e->st, strslice("Num", 3)));
    if (!num) return NULL;
    if (!elab_num_instance_matches(e, num, first_t)) return NULL;

    char dotbuf[8];
    int dotlen = snprintf(dotbuf, sizeof(dotbuf), ".%s", method);
    if (dotlen <= 0 || (size_t)dotlen >= sizeof(dotbuf)) return NULL;
    const Symbol *dot_sym =
        symtab_intern(e->st, strslice(dotbuf, (uint32_t)dotlen));

    if (n_args == 1) {
        Form **items = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
        items[0] = form_sym(e->arena, head->span, dot_sym);
        items[1] = call->as.list.items[1];
        return elab_method_call(e, form_list(e->arena, call->span, items, 2));
    }

    Form *acc = call->as.list.items[1];
    for (uint32_t i = 2; i <= n_args; i++) {
        Form **items = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
        items[0] = form_sym(e->arena, head->span, dot_sym);
        items[1] = acc;
        items[2] = call->as.list.items[i];
        acc = form_list(e->arena, call->span, items, 3);
    }
    return elab_method_call(e, acc);
}

/* True when a TY_FN receiver carries a float-class value in any argument or in
 * its result.  Used to keep float-carrier function composition (e.g. a float
 * `>>>` pipeline) on the register-class-correct free defn instead of the
 * type-erased (->) typeclass instance method -- see
 * docs/archive/history/sf-compose-typed-arrow-prints-garbage-floats.md.  The
 * (->) instance body is emitted once with an int64 carrier thunk (rax), so a
 * float carrier (xmm0) would be a register-class miscompile that only works by
 * luck; the free typed combinator specializes per-carrier and is correct. */
static bool fn_type_has_float_carrier(const Type *t) {
    if (!t || t->kind != TY_FN) return false;
    TypeKind rk = t->as.fn.result_kind;
    if (rk == TY_FLOAT || rk == TY_FLOAT32 || rk == TY_FLOAT64) return true;
    for (uint32_t i = 0; i < t->as.fn.arity; i++) {
        TypeKind ak = t->as.fn.arg_kinds[i];
        if (ak == TY_FLOAT || ak == TY_FLOAT32 || ak == TY_FLOAT64) return true;
    }
    return false;
}

/* Method/defn namespace separation (fix (1) of
 * docs/archive/history/typeclass-methods-share-value-namespace-with-defns.md).
 *
 * True when `name` is a method of a *user-defined* (non-stdlib) typeclass AND
 * some instance of that class matches the receiver type `recv`.  When this
 * holds, a bare `(m x ...)` whose head also binds a free `defn` of the same
 * name prefers typeclass dispatch over the free defn -- letting a class method
 * and a same-named free helper coexist in one module (the Arrow case: a bare
 * `arr` combinator alongside the `Arrow` method `arr`).
 *
 * Stdlib class methods are intentionally excluded (`from_stdlib`): the
 * documented pattern of a user `defn` overriding a stdlib method (e.g. a local
 * `show`) must keep "defn wins".  The match test mirrors the exact-match arm of
 * elab_method_call's instance search, so we only redirect when dispatch would
 * genuinely resolve; an unknown/unmatched receiver keeps the free defn. */
static bool elab_user_method_instance_matches(Elab *e, const Symbol *name,
                                              const Type *recv) {
    if (!name || !recv) return false;
    TypeKind rk = recv->kind;
    if (rk == TY_UNKNOWN) return false;          /* undecidable -> keep the defn */
    bool recv_primitive = (rk == TY_INT  || rk == TY_BOOL  || rk == TY_CSTR ||
                           rk == TY_NIL  || rk == TY_FLOAT || rk == TY_PTR_VOID ||
                           rk == TY_SYM);
    for (TypeClassInstance *inst = e->typeclass_env.instances; inst; inst = inst->next) {
        TypeClass *tc = inst->typeclass;
        if (!tc || tc->from_stdlib) continue;
        bool name_match = false;
        for (uint8_t mi = 0; mi < tc->n_methods; mi++) {
            const Symbol *mn = tc->methods[mi].name;
            if (mn && mn->len == name->len &&
                memcmp(mn->name, name->name, name->len) == 0) { name_match = true; break; }
        }
        if (!name_match) continue;
        if (inst->n_type_args == 0) return true;  /* no dispatch arg -> name suffices */
        TypeKind itk = inst->type_args[0].kind;
        if (recv_primitive) {
            if (itk == rk) return true;
            continue;
        }
        /* Receiver is non-primitive (KIND_ARROW receiver). */
        bool inst_primitive = (itk == TY_INT  || itk == TY_BOOL  || itk == TY_CSTR ||
                               itk == TY_NIL  || itk == TY_FLOAT || itk == TY_PTR_VOID ||
                               itk == TY_SYM);
        if (inst_primitive) continue;
        if (itk == TY_FN || rk == TY_FN) {
            if (itk == TY_FN && rk == TY_FN) {
                /* sf-compose-typed: the function-arrow (->) instance method is
                 * emitted with an int64 carrier thunk and cannot carry a float
                 * value correctly.  When the receiver is a concrete function
                 * with a float-class carrier and a same-named free defn exists
                 * (this matcher is only consulted when one does), keep the call
                 * on the register-class-correct free defn instead of routing to
                 * the int64-carrier instance method. */
                if (fn_type_has_float_carrier(recv)) continue;
                return true;
            }
            continue;
        }
        return true;   /* both non-primitive, no discriminator -> accept */
    }
    return false;
}

/* Phase R6b: true if [p, p+n) contains the substring `needle`. */
static bool lint_line_has_marker(const char *p, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m == 0 || n < m) return false;
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(p + i, needle, m) == 0) return true;
    }
    return false;
}

/* Phase R6b: --lint-panic allow-list. A `;; #lint-panic-allow` comment in the
 * file's leading comment block silences the whole file; the same comment on
 * the line immediately preceding a call silences just that call. Returns true
 * if the call at `span` is allow-listed. */
static bool lint_panic_allowed(Span span) {
    const SourceFile *f = diag_source_file(span.file_id);
    if (!f || !f->src) return false;
    const char *src = f->src;
    size_t len = f->len;
    static const char marker[] = "#lint-panic-allow";

    /* File-level: scan the leading run of blank/comment lines. If the marker
     * appears before the first code line, the whole file is allow-listed. */
    {
        size_t i = 0;
        while (i < len) {
            size_t ls = i;
            while (i < len && src[i] != '\n') i++;
            size_t le = i;
            if (i < len) i++;
            size_t s = ls;
            while (s < le && (src[s] == ' ' || src[s] == '\t')) s++;
            if (s == le) continue;            /* blank line */
            if (src[s] == ';') {              /* comment line */
                if (lint_line_has_marker(src + s, le - s, marker)) return true;
                continue;
            }
            break;                            /* first code line -> stop */
        }
    }

    /* Per-call: examine the immediately-preceding non-blank line. */
    {
        size_t pos = span.off_start;
        if (pos > len) pos = len;
        while (pos > 0 && src[pos - 1] != '\n') pos--;  /* start of call's line */
        while (pos > 0) {
            size_t end = pos - 1;             /* '\n' ending the previous line */
            size_t ls = end;
            while (ls > 0 && src[ls - 1] != '\n') ls--;
            size_t s = ls;
            while (s < end && (src[s] == ' ' || src[s] == '\t')) s++;
            if (s == end) { pos = ls; continue; }   /* blank -> keep looking up */
            if (src[s] == ';')
                return lint_line_has_marker(src + s, end - s, marker);
            return false;                     /* non-comment code line -> no allow */
        }
    }
    return false;
}

/* Phase R6b: panic-site names flagged by --lint-panic. */
static bool lint_is_panic_site(const char *nm, bool *is_unwrap_out) {
    bool is_unwrap = (strcmp(nm, "result-unwrap") == 0 ||
                      strcmp(nm, "option-unwrap") == 0);
    *is_unwrap_out = is_unwrap;
    return is_unwrap ||
        strcmp(nm, "panic") == 0          || strcmp(nm, "tur_panic") == 0 ||
        strcmp(nm, "assert!") == 0        || strcmp(nm, "assert-msg!") == 0 ||
        strcmp(nm, "require!") == 0       || strcmp(nm, "require-msg!") == 0 ||
        strcmp(nm, "ensure!") == 0        || strcmp(nm, "ensure-msg!") == 0 ||
        strcmp(nm, "invariant!") == 0     || strcmp(nm, "invariant-msg!") == 0;
}

/* structdef-retirement DS-D: synthesize_struct_ctor (the CURRY-V2 backing
 * constructor synthesizer for by-value structs) is deleted -- its only caller
 * was gated on a binding whose type had kind TY_STRUCT, which never occurs now
 * that structs lower to record ADTs. */


/* D8 Q3 (the generic-parameter widen): does `t` mention the type variable
 * `name` anywhere -- as itself, inside an application, or inside a fn type? */
static bool type_mentions_tyvar_named(const Type *t, const char *name) {
    if (!t || !name) return false;
    switch (t->kind) {
        case TY_TYVAR:
            return t->as.tyvar_.name && strcmp(t->as.tyvar_.name, name) == 0;
        case TY_APP:
            return type_mentions_tyvar_named(t->as.app.fn, name) ||
                   type_mentions_tyvar_named(t->as.app.arg, name);
        case TY_FN:
            if (t->as.fn.result_full_type &&
                type_mentions_tyvar_named(t->as.fn.result_full_type, name)) return true;
            if (t->as.fn.arg_full_types)
                for (uint32_t i = 0; i < t->as.fn.arity; i++)
                    if (type_mentions_tyvar_named(t->as.fn.arg_full_types[i], name)) return true;
            return false;
        default:
            return false;
    }
}


/* D8 Q3 (Saffron result grounding): collect the distinct named type variables
 * mentioned anywhere in `t`, up to `cap`. */
static void call_collect_tyvar_names(const Type *t, const char **names, uint8_t *n, uint8_t cap) {
    if (!t || *n >= cap) return;
    switch (t->kind) {
        case TY_TYVAR:
            if (t->as.tyvar_.name) {
                for (uint8_t i = 0; i < *n; i++)
                    if (strcmp(names[i], t->as.tyvar_.name) == 0) return;
                names[(*n)++] = t->as.tyvar_.name;
            }
            return;
        case TY_APP:
            call_collect_tyvar_names(t->as.app.fn, names, n, cap);
            call_collect_tyvar_names(t->as.app.arg, names, n, cap);
            return;
        case TY_FN:
            if (t->as.fn.arg_full_types)
                for (uint32_t i = 0; i < t->as.fn.arity; i++)
                    call_collect_tyvar_names(t->as.fn.arg_full_types[i], names, n, cap);
            call_collect_tyvar_names(t->as.fn.result_full_type, names, n, cap);
            return;
        default:
            return;
    }
}

static Expr *elab_call_inner(Elab *e, Form *call);

/* Stack backstop for nested-call elaboration.
 *
 * elab_call -> elab_form -> elab_call recurses once per level of expression
 * nesting, and until now nothing bounded it at all: a deeply nested expression
 * did not get a diagnostic, it aborted the compiler
 * (`AddressSanitizer: stack-overflow ... in elab_call`).  The emitter's walk
 * had the same shape and at least reported before dying; this one did not.
 *
 * The bound is the real stack, not a constant -- the same reasoning as the
 * emitter's guard and the macro-expansion guard below, and the same shared
 * helper.  With the driver running on a TUR_STACK_MB-sized stack (main.c) this
 * is genuinely a backstop: reaching it means a walk that is unbounded rather
 * than merely deep.
 *
 * The counter exists to hold the floor and to name a depth in the message; it
 * is not a cap.  A wrapper carries it because elab_call_inner has too many
 * return paths to bracket by hand. */
#define ELAB_CALL_DEPTH_STACK_FLOOR 8
static int  g_elab_call_depth;
static bool g_elab_call_depth_reported;

Expr *elab_call(Elab *e, Form *call) {
    if (g_elab_call_depth == 0) g_elab_call_depth_reported = false;
    if (g_elab_call_depth >= ELAB_CALL_DEPTH_STACK_FLOOR &&
        tur_stack_nearly_exhausted()) {
        if (!g_elab_call_depth_reported) {
            size_t total = 0;
            (void)tur_stack_headroom(&total);
            g_elab_call_depth_reported = true;
            diag_emit_with_code(DIAG_ERROR, call->span,
                                TUR_E0712_EXPR_NESTING_TOO_DEEP,
                                "expression nesting exhausted the compiler's "
                                "stack at depth %d (%zu MiB)",
                                g_elab_call_depth,
                                total / (size_t)(1024 * 1024));
            diag_emit(DIAG_NOTE, call->span,
                      "raise it with TUR_STACK_MB (default %d), or split the "
                      "expression into helper functions or `let` bindings; if "
                      "a macro generated it, have the macro emit a flatter "
                      "form",
                      TUR_STACK_MB_DEFAULT);
        }
        return NULL;
    }
    g_elab_call_depth++;
    Expr *r = elab_call_inner(e, call);
    g_elab_call_depth--;
    return r;
}

static Expr *elab_call_inner(Elab *e, Form *call) {
    /* Already established: call->tag == F_LIST and len >= 1. */
    Form *head = call->as.list.items[0];

    /* docs/archive/history/list-macro-quote-vs-syntactic-symbol.md: a macro that
     * builds an expansion via `(list 'foo args...)` lands here with the head
     * shaped as F_QUOTE wrapping F_SYM(foo) -- the same `'foo` literal the
     * macro body wrote. Without this unwrap the head elaborates to an
     * EX_SYM_LIT (:Sym), which falls through to the head-expression path and
     * trips "expression in call head has type `Sym`, which is not callable".
     * Treat `'sym` in head position the same as the bare `sym`: rewrite the
     * call's head to the underlying F_SYM and recurse so scope lookup,
     * special-form dispatch, and macro dispatch all fire as expected. This
     * restores the round-trip for macro authors who carry symbols around as
     * quoted values and splice them into call-head positions. */
    if (head->tag == F_QUOTE && head->as.list.len == 1 &&
        head->as.list.items[0]->tag == F_SYM) {
        uint32_t n = call->as.list.len;
        Form **items = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
        items[0] = head->as.list.items[0];
        for (uint32_t i = 1; i < n; i++) items[i] = call->as.list.items[i];
        Form *rewritten = form_list(e->arena, call->span, items, n);
        return elab_call(e, rewritten);
    }

    /* General callable-expression heads: ((expr) args...). */
    if (head->tag != F_SYM) {
        Expr *head_expr = elab_form(e, head);
        if (!head_expr) return NULL;
        return elab_call_head_expr(e, call, head_expr);
    }
    const Symbol *name = head->as.sym;

    /* saffron-match-any-scrutinee-on-parametric-adt-emits-bad-c, fix direction
     * 1: in a Saffron file a PARAMETRIC constructor's type-variable fields are
     * built at `any`, so `(Sq 6)` is a `(Shape any)` rather than a `(Shape
     * int)`.
     *
     * This is what makes `(match s (Sq w) ...)` work on an unannotated
     * scrutinee. The `any`-scrutinee narrow (elab_structs.c) unboxes to the ADT
     * the arms name; for a parametric ADT that target has to be the ADT applied
     * to something, and `any` is the only choice a match site can make. Before
     * this, the narrow declined outright (`n_type_params == 0`) and the emitter
     * cast the 16-byte `tur_tagged_t` straight to a pointer -- uncompilable C.
     * Relaxing the guard alone was tried and reverted: the narrow then fired and
     * panicked `cast: any holds a different instantiation of Shape`, because the
     * box held what the CONSTRUCTOR produced. This is the other half.
     *
     * It is the same answer S6 already gives container literals
     * (dl_saffron_widen_elems, elab_toplevel.c) -- and spelled the same way, as
     * an `(:: arg any)` ascription on the FORM, before anything is elaborated,
     * so the ordinary unifier does the work and no inference path learns a
     * dialect-dependent second mode. It makes "an undetermined type argument is
     * `any` in Saffron" true uniformly, instead of true for containers only.
     *
     * Only fields DECLARED as a type variable widen: a concrete field in a
     * parametric ADT (`(defdata Box [a] (Tagged :int a))`) keeps its type, so
     * the ascription cannot silently erase an annotation the author wrote. And
     * only a saturated call -- an under-applied ctor partial-applies into a
     * closure, and ascribing its arguments would change what that closure
     * completes to. */
    /* saffron-dynamic-surface-pass (low): the widen defers to an enclosing
     * TY_APP expectation because `(:: (Wrap 7) (W int))` pins the
     * instantiation on purpose -- but an expectation whose every type
     * argument is ITSELF `any` (a `[v : (W any)]` parameter, the seam into
     * an all-`any` slot) pins nothing the widen would not choose anyway, and
     * declining under it built a `(W int)` that the parameter then rejected
     * (`expected (type-app W any), got (type-app W int)`), while the same
     * `(Wrap 7)` through an unannotated defn was accepted.  So the rule keys
     * on what the expectation SAYS, not on its presence: only a concrete
     * argument somewhere in the applied type pins. */
    if (lang_span_is_dynamic(call->span) &&
        !saffron_expected_app_pins(e->expected_type)) {
        CtorDef *sctor = elab_lookup_ctor(e, name);
        if (sctor && sctor->adt && sctor->adt->n_type_params > 0 &&
            call->as.list.len - 1u == sctor->n_fields) {
            bool any_tyvar = false;
            for (uint32_t i = 0; i < sctor->n_fields; i++)
                if (sctor->fields[i].full_type &&
                    sctor->fields[i].full_type->kind == TY_TYVAR) {
                    any_tyvar = true; break;
                }
            if (any_tyvar) {
                const Symbol *any_sym = symtab_intern(e->st, strslice("any", 3));
                const Symbol *asc_sym = symtab_intern(e->st, strslice("::", 2));
                uint32_t n = call->as.list.len;
                Form **items = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
                items[0] = call->as.list.items[0];
                for (uint32_t i = 1; i < n; i++) {
                    Form *arg = call->as.list.items[i];
                    const CtorField *f = &sctor->fields[i - 1];
                    if (!f->full_type || f->full_type->kind != TY_TYVAR) {
                        items[i] = arg;
                        continue;
                    }
                    Form *asc[3] = { form_sym(e->arena, arg->span, asc_sym), arg,
                                     form_sym(e->arena, arg->span, any_sym) };
                    items[i] = form_list(e->arena, arg->span, asc, 3);
                }
                call = form_list(e->arena, call->span, items, n);
            }
        }
    }

    /* D8 Q3, the rule the ctor widen above already states, extended to a
     * GENERIC FUNCTION's bare type-variable parameters: in a Saffron file an
     * undetermined type argument is `any`.
     *
     * `(some 41)` is not a constructor call -- `some` is a stdlib defn,
     * `[A] [x : A] : (Option A)` -- so the ctor widen never saw it and a
     * Saffron program built an `(Option int)`.  That is the ONE instantiation
     * dynamic dispatch cannot serve: an `(Option int)` holds raw ints, and a
     * Saffron closure handed to `.fmap` expects boxes, so the closure's
     * `(fn [any] any)` can never be applied to its elements.  Every value a
     * Saffron file builds through a generic constructor function therefore has
     * to be the all-`any` instantiation, exactly as one built through a
     * literal (S6) or a raw ctor (above) already is.  Consistency, not a new
     * rule: the dialect's default type applies wherever the program left a
     * type undetermined.
     *
     * Only a BARE type variable widens.  An applied type mentioning one
     * (`(Vec A)`) is grounded by the D5 seam at the call, and a concrete
     * parameter keeps its type.  Ascribing to `any` is a boxing no-op on a
     * value that is already `any`, so a symbol bound to one is unaffected. */
    /* Both widens below defer to an ENCLOSING ascription: `(:: (some 3.5)
     * (Option float))` pins the instantiation on purpose, and the ascription
     * sets e->expected_type around the inner form -- the same signal the ctor
     * result path consults.  Widening under it produced an `(Option any)` the
     * ascription then could not accept. */
    bool saffron_pinned = saffron_expected_app_pins(e->expected_type);
    if (lang_span_is_dynamic(call->span) && !saffron_pinned && !elab_lookup_ctor(e, name)) {
        Binding *gb = scope_lookup(e->scope, name);
        if (gb && gb->type.kind == TY_FN && gb->type.as.fn.arg_full_types &&
            gb->type.as.fn.arity > 0) {
            uint32_t n_provided = call->as.list.len - 1;
            uint32_t arity = gb->type.as.fn.arity;
            uint32_t n_look = n_provided < arity ? n_provided : arity;
            /* "Nothing else can pin it": a bare-tyvar parameter widens only
             * when its variable appears in NO compound parameter type of the
             * callee.  `unwrap-or [o : (Option A) d : A]` keeps `d` as written
             * -- `o` fixes `A`, and widening `d` to `any` against an `(Option
             * float)` receiver was a type error on the guide's own type-case
             * example.  `some [x : A]` and `identity [x : A]` widen, because
             * their `A` has no other source.  A property of the SIGNATURE, so
             * it is decidable here on the forms, in any argument order. */
            bool widen_param[64] = {0};
            bool any_bare = false;
            for (uint32_t i = 0; i < n_look && i < 64; i++) {
                if (saffron_bare_tyvar_param_widens(&gb->type, i)) {
                    widen_param[i] = true;
                    any_bare = true;
                }
            }
            if (any_bare) {
                const Symbol *any_sym = symtab_intern(e->st, strslice("any", 3));
                const Symbol *asc_sym = symtab_intern(e->st, strslice("::", 2));
                uint32_t n = call->as.list.len;
                Form **items = (Form **)arena_alloc(e->arena, n * sizeof(Form *));
                items[0] = call->as.list.items[0];
                for (uint32_t i = 1; i < n; i++) {
                    Form *arg = call->as.list.items[i];
                    const Type *ft = (i - 1 < n_look && i - 1 < 64 && widen_param[i - 1])
                                         ? gb->type.as.fn.arg_full_types[i - 1] : NULL;
                    bool already = arg->tag == F_LIST && arg->as.list.len == 3 &&
                                   arg->as.list.items[0]->tag == F_SYM &&
                                   arg->as.list.items[0]->as.sym == asc_sym &&
                                   arg->as.list.items[2]->tag == F_SYM &&
                                   arg->as.list.items[2]->as.sym == any_sym;
                    if (!ft || ft->kind != TY_TYVAR || already) { items[i] = arg; continue; }
                    Form *asc[3] = { form_sym(e->arena, arg->span, asc_sym), arg,
                                     form_sym(e->arena, arg->span, any_sym) };
                    items[i] = form_list(e->arena, arg->span, asc, 3);
                }
                call = form_list(e->arena, call->span, items, n);
            }
        }
    }

    /* Phase C2: --no-contracts strips contract checks before their arguments
     * are elaborated, so the predicate expression (and any side effects it
     * carries) never run -- matching the Rust/C `assert` convention. The
     * `assert!`/`require!`/`ensure!`/`invariant!` macros expand to calls to
     * `tur-contract-check` / `tur-contract-check-inv`; we drop those calls
     * here and fold `contract-enabled?` to `false`. */
    if (g_no_contracts) {
        const Symbol *cc  = symtab_intern(e->st, strslice("tur-contract-check", 18));
        const Symbol *cci = symtab_intern(e->st, strslice("tur-contract-check-inv", 22));
        const Symbol *ce  = symtab_intern(e->st, strslice("contract-enabled?", 17));
        if (name == cc || name == cci) {
            /* Void no-op: contract checks are `:void`, which lowers to TY_NIL. */
            return expr_new(e->arena, EX_NIL_LIT, TYPE_NIL, call->span);
        }
        if (name == ce && call->as.list.len == 1) {
            Expr *f = expr_new(e->arena, EX_BOOL_LIT, TYPE_BOOL, call->span);
            f->as.b = false;
            return f;
        }
    }

    /* Phase R6b: --lint-panic warns at panic call sites (panic/tur_panic, the
     * contract macros, and result-unwrap/option-unwrap) unless allow-listed by
     * a `;; #lint-panic-allow` comment. The macro names are still visible here
     * because macro expansion happens later in this function. result-unwrap /
     * option-unwrap carry a soft-deprecation hint toward *-must (OQ#1). */
    if (g_lint_panic) {
        bool is_unwrap = false;
        if (lint_is_panic_site(name->name, &is_unwrap) &&
            !lint_panic_allowed(call->span)) {
            if (is_unwrap) {
                diag_emit_with_code(DIAG_WARNING, call->span, TUR_W0038_LINT_PANIC_SITE,
                    "panic call site '%s' outside allow-list; "
                    "prefer result-must / option-must", name->name);
            } else {
                diag_emit_with_code(DIAG_WARNING, call->span, TUR_W0038_LINT_PANIC_SITE,
                    "panic call site '%s' outside allow-list", name->name);
            }
        }
    }

    /* SZ7: static size checking -- reject statically-known size mismatches at
     * compile time before normal call dispatch. */
    if (sz7_static_size_violation(e, call, name)) return NULL;

    /* Special forms. */
    /* def/define consolidation D1: one form, two spellings.  Position, not
     * spelling, selects the behaviour -- a body window is rewritten to a `let`
     * by splice_internal_defines before dispatch ever gets here, so anything
     * reaching elab_def is either the top level (a global binding) or an
     * expression position (the §3.3 diagnostic). */
    if (name == e->sym_def || name == e->sym_define) return elab_def(e, call);
    if (name == e->sym_let)    return elab_let   (e, call);
    if (name == e->sym_letstar) return elab_letstar(e, call);
    if (name == e->sym_letrec) return elab_letrec(e, call);
    if (name == e->sym_if)     return elab_if    (e, call);
    if (name == e->sym_do)     return elab_do    (e, call);
    if (name == e->sym_unsafe) return elab_unsafe(e, call);
    if (name == e->sym_set)    return elab_set   (e, call);
    if (name == e->sym_while)  return elab_while (e, call);
    if (name == e->sym_case)   return elab_case  (e, call);
    /* Phase 4 */
    if (name == e->sym_defer)  return elab_defer (e, call);
    if (name == e->sym_return) return elab_return(e, call);
    /* proper-tail-calls T1: `(^tailcall <call>)` -- checked tail-call marker. */
    if (name == e->sym_tailcall) return elab_tailcall(e, call);
    /* GF1: Generator forms */
    if (name == e->sym_gen)      return elab_gen     (e, call);
    if (name == e->sym_yield)    return elab_yield   (e, call);
    if (name == e->sym_gen_next) return elab_gen_next(e, call);
    if (name == e->sym_gen_done) return elab_gen_done(e, call);
    if (name == e->sym_gen_unwrap && call->as.list.len == 2)
        return elab_gen_unwrap(e, call);
    /* Phase 5 */
    if (name == e->sym_ref)    return elab_ref   (e, call);
    if (name == e->sym_deref)  return elab_deref (e, call);
    if (name == e->sym_drop)   return elab_drop  (e, call);
    /* LT3: lref<T> */
    if (name == e->sym_lref_new) return elab_lref_new(e, call);
    /* Phase 9: rc<T> + weak<T> */
    if (name == e->sym_rc_of)       return elab_rc_of(e, call);
    if (name == e->sym_rc_clone)    return elab_rc_clone(e, call);
    if (name == e->sym_rc_drop)     return elab_rc_drop(e, call);
    if (name == e->sym_rc_ptr)      return elab_rc_ptr(e, call);
    if (name == e->sym_rc_strong_count) return elab_rc_strong_count(e, call);
    if (name == e->sym_rc_from_ref) return elab_rc_from_ref(e, call);
    if (name == e->sym_ref_from_rc) return elab_ref_from_rc(e, call);
    if (name == e->sym_weak)        return elab_weak(e, call);
    if (name == e->sym_upgrade)     return elab_weak_upgrade(e, call);
    if (name == e->sym_weak_pred)   return elab_weak_pred(e, call);
    if (name == e->sym_ref_pred)    return elab_ref_pred(e, call);
    /* Phase 18: Delimited continuations */
    if (name == e->sym_reset)      return elab_reset(e, call);
    if (name == e->sym_shift)      return elab_shift(e, call);
    if (name == e->sym_shift0)     return elab_shift0(e, call);
    if (name == e->sym_call_cc)    return elab_call_cc(e, call);
    if (name == e->sym_escape)     return elab_escape(e, call);
    /* Phase B2: Cloneable continuations */
    if (name == e->sym_cloneable_reset)  return elab_cloneable_reset(e, call);
    if (name == e->sym_cloneable_shift)  return elab_cloneable_shift(e, call);
    /* Note: the interim `k-reset`/`k-shift` spellings were retired once plain
     * `shift`/`reset` became the unified surface (item d) -- a `cont`-typed
     * receiver routes `shift` to the continuation-passing path automatically. */
    if (name == e->sym_call_cc_star)      return elab_call_cc_star(e, call);
    /* Phase 21: Serializable continuations */
    if (name == e->sym_serial_reset) return elab_serial_reset(e, call);
    if (name == e->sym_serial_shift) return elab_serial_shift(e, call);
    /* DV0-DV1: Dynamic vars */
    if (name == e->sym_defdynamic) return elab_defdynamic(e, call);
    if (name == e->sym_binding)    return elab_binding   (e, call);
    /* Phase 19: Algebraic effects */
    if (name == e->sym_defeffect) return elab_defeffect(e, call);
    if (name == e->sym_perform)   return elab_perform(e, call);
    if (name == e->sym_handle)       return elab_handle(e, call);
    if (name == e->sym_handle_shallow) return elab_handle_shallow(e, call);
    if (name == e->sym_try_with)     return elab_try_with(e, call);
    /* FH2: (handler (E [params] k) body) in value position is a handler literal.
     * A local binding named `handler` (e.g. a higher-order function parameter)
     * shadows the special form and is dispatched as an ordinary call. */
    if (name == e->sym_handler_type && !scope_lookup(e->scope, name))
        return elab_handler_lit(e, call);
    /* FH3: (with-handler hv body) -- exactly two args -- applies a handler value.
     * Any other arity is the T25 inline-handle sugar (body + case/body pairs). */
    if (name == e->sym_with_handler) {
        if (call->as.list.len == 3) return elab_with_handler(e, call);
        return elab_handle(e, call);  /* T25: sugar for handle in async context */
    }
    if (name == e->sym_resume)    return elab_resume(e, call);
    if (name == e->sym_discontinue) return elab_discontinue(e, call);
    /* ET3-E: compose-handlers */
    if (name == e->sym_compose_handlers) return elab_compose_handlers(e, call);
    if (name == e->sym_cont_pred)   return elab_cont_pred(e, call);
    /* Phase 10: GC */
    if (name == e->sym_gc_force)    return elab_gc_force(e, call);
    if (name == e->sym_gc_enable)   return elab_gc_enable(e, call);
    if (name == e->sym_gc_disable)  return elab_gc_disable(e, call);
    if (name == e->sym_gc_auto)     return elab_gc_auto(e, call);
    if (name == e->sym_gc_collections)   return elab_gc_collections(e, call);
    if (name == e->sym_gc_objects_freed) return elab_gc_objects_freed(e, call);
    if (name == e->sym_gc_live_blocks)   return elab_gc_live_blocks(e, call);
    if (name == e->sym_gc_cand_hw)       return elab_gc_candidate_high_water(e, call);
    /* Phase M0: Module system */
    if (name == e->sym_load)      return elab_load(e, call);
    if (name == e->sym_defmodule) return elab_defmodule(e, call);
    if (name == e->sym_export) {
        diag_emit(DIAG_ERROR, call->span,
                  "export is only allowed inside defmodule");
        return NULL;
    }
    if (name == e->sym_import) {
        diag_emit(DIAG_ERROR, call->span,
                  "import is only allowed inside defmodule");
        return NULL;
    }
    /* Phase N: numeric cast */
    if (name == e->sym_as) return elab_as_cast(e, call);
    /* IT4: gradual typing */
    if (name == e->sym_type_of) return elab_any_type_of(e, call);
    if (name == e->sym_cast)    return elab_any_cast(e, call);
    if (name == e->sym_is_q)    return elab_is_q(e, call);
    /* Phase 11: defstruct */
    if (name == e->sym_defstruct) return elab_defstruct(e, call);
    if (name == e->sym_make_struct) return elab_make_struct(e, call);
    /* WITH-V0: functional struct update.  Gated on `!scope_lookup` so a user
     * binding named `with` still wins (mirrors the session-op shadowing rule). */
    if (name == e->sym_with && !scope_lookup(e->scope, name))
        return elab_with(e, call);
    /* CTOR-V0: auto-bound struct constructor call syntax `(Name args...)`.
     * When `name` resolves to a struct type binding -- disambiguated from value
     * bindings by scope_lookup_type_def, which filters TY_STRUCT/TY_ADT -- the
     * call is rewritten to `(make-struct Name args...)`.  This covers both the
     * positional form `(Name a b)` and the keyword form `(Name :f v ...)`
     * (make-struct reorders keyword pairs into field order and diagnoses
     * missing/unknown/duplicate/mixed args).  `:no-auto-ctor` opts out, leaving
     * `(Name ...)` to resolve as an ordinary -- and here, non-callable -- value.
     *
     * CURRY-V2 (struct-return-through-closure-loses-type, now resolved): an
     * under-applied *positional* constructor curries -- `(Name a)` for a
     * 2-field struct partial-applies a synthesized backing constructor and
     * yields a closure that completes to the struct.  The by-value struct
     * result now flows through the closure ABI (CURRY-V0/V1).  Full
     * applications and the keyword form keep the direct make-struct fast path;
     * parameterized structs decline currying (the synthesizer returns NULL). */
    /* structdef-retirement slice 2 (CTOR-V0): a `:no-auto-ctor` lowered record ADT
     * keeps its ctor binding (make-struct needs it) but rejects a DIRECT
     * `(Name ...)` call -- exactly the struct path's `!no_auto_ctor` gate below,
     * ported to the ADT path.  make-struct's rewrite sets make_struct_ctor_rewrite
     * to let its own call through; read-and-clear so a nested user `(Name ...)` in
     * an arg is still rejected. */
    {
        bool ms_rewrite = e->make_struct_ctor_rewrite;
        e->make_struct_ctor_rewrite = false;
        if (!ms_rewrite) {
            Binding *tb = scope_lookup_type_def(e->scope, name);
            if (tb && tb->type.kind == TY_ADT && tb->type.as.adt_.def &&
                tb->type.as.adt_.def->no_auto_ctor) {
                AdtDef *nac = tb->type.as.adt_.def;
                Binding *nearest = scope_lookup(e->scope, name);
                bool is_ctor_head = nearest &&
                    ((nearest->type.kind == TY_ADT &&
                      nearest->type.as.adt_.def == nac) ||
                     nearest->type.kind == TY_FN);
                if (is_ctor_head) {
                    diag_emit(DIAG_ERROR, head->span,
                              "'%s' is not a function or continuation",
                              name->name);
                    return NULL;
                }
            }
        }
    }
    /* structdef-retirement DS-D: the direct `(Name ...)` struct-constructor
     * routing block (gated on the nearest binding being a TY_STRUCT type def)
     * is dead -- no binding's type ever has kind TY_STRUCT; a struct name is a
     * record ADT and routes through the ADT constructor path. */
    /* M2b: (default-of T) is a builtin zero-value form, BUT a user program may
     * legitimately declare a typeclass method named `default-of` (the canonical
     * return-position-only dispatch example).  When such a class is in scope,
     * let the method shadow the builtin: fall through so the return-dispatch
     * path below resolves it from the expected-type channel.  Stdlib never
     * declares a `default-of` class, so its `(default-of A)` make-struct payload
     * fills still hit the builtin.  See root cause B of
     * docs/archive/history/m5-suite-residual-6-failures-2026-06-14.md. */
    if (name == e->sym_default_of && !elab_name_is_typeclass_method(e, name))
        return elab_default_of(e, call);
    /* SI4-C: defopaque */
    if (name == e->sym_defopaque) return elab_defopaque(e, call);
    /* Phase G0: ADTs */
    if (name == e->sym_defdata) return elab_defdata(e, call);
    if (name == e->sym_match) return elab_match(e, call);
    if (name == e->sym_defgadt) return elab_defgadt(e, call);
    if (name == e->sym_coerce)  return elab_coerce(e, call);
    /* Phase 12: Borrow traits */
    if (name == e->sym_borrow) return elab_borrow_immut(e, call);
    if (name == e->sym_borrow_mut) return elab_borrow_mut(e, call);
    /* Phase 15: Typeclasses */
    if (name == e->sym_defclass) return elab_defclass(e, call);
    if (name == e->sym_definstance) return elab_definstance(e, call);
    /* Phase HKT H5: kind aliases */
    if (name == e->sym_defkind) return elab_defkind(e, call);
    /* Phase HKT-P2: recursive type binders */
    if (name == e->sym_defrec) return elab_defrec(e, call);
    if (name == e->sym_deftype) return elab_deftype(e, call);
    /* Phase TA1: defalias */
    if (name == e->sym_defalias) return elab_defalias(e, call);
    /* Phase HRT0: forall/exists are type-level forms; reject in expression position */
    if (name == e->sym_forall || name == e->sym_forall_u) {
        diag_emit(DIAG_ERROR, call->span,
                  "'forall' is a type-level annotation and cannot appear in expression position "
                  "(use it in a type annotation: (deftype MyType (forall [a] ...)))");
        return NULL;
    }
    if (name == e->sym_exists || name == e->sym_exists_u) {
        diag_emit(DIAG_ERROR, call->span,
                  "'exists' is a type-level annotation and cannot appear in expression position "
                  "(use it in a type annotation: (deftype MyType (exists [a] ...)))");
        return NULL;
    }
    /* Phase HKT-P1: type-level application */
    if (name == e->sym_type_app) return elab_type_app(e, call);
    /* Phase HRT1: (:: expr type) — type ascription */
    if (name == e->sym_ascribe) return elab_ascribe(e, call);
    /* toplevel-def-initializers-run-before-toplevel-expressions:
     * `(__tur-deferred-init__ name)` IS the `^deferred-init` def's
     * initializer, taken from the binding once, so `(set! name
     * (__tur-deferred-init__ name))` runs it at its source position with the
     * def's own static type. */
    if (name == e->sym_deferred_init) {
        if (call->as.list.len != 2 || call->as.list.items[1]->tag != F_SYM) {
            diag_emit(DIAG_ERROR, call->span, "__tur-deferred-init__ takes one binding name");
            return NULL;
        }
        Binding *b = scope_lookup(e->scope, call->as.list.items[1]->as.sym);
        if (!b || !b->deferred_init) {
            diag_emit(DIAG_ERROR, call->span,
                      "__tur-deferred-init__: '%s' has no deferred initializer to run "
                      "(not a `^deferred-init` def, or already run)",
                      call->as.list.items[1]->as.sym->name);
            return NULL;
        }
        Expr *init = b->deferred_init;
        b->deferred_init = NULL;
        return init;
    }
    /* Phase HRT2: existential types */
    if (name == e->sym_pack) return elab_pack(e, call);
    if (name == e->sym_open) return elab_open(e, call);
    /* SS0b: Session channel operations (-Xsessions, now always-on).
     *
     * always-on-linear-session-fixture-failures (theme 3): with sessions
     * unconditionally enabled, the value-level ops `send`/`recv`/`close`/...
     * are no longer behind an opt-in flag, so they intercept calls in
     * programs that never use session types.  A program is free to define
     * its own ordinary function named `recv` (e.g. a generic channel
     * forwarder over a user `defopaque SChan`); that user binding must win
     * over the keyword.  Gate each value-level op on `!scope_lookup` so a
     * shadowing user/global binding falls through to normal call resolution
     * -- the same shadowing discipline already applied to `handler-type`
     * (above) and `default-of` (below).  Session ops dispatch by pure symbol
     * identity and have no backing binding, so `scope_lookup` only ever
     * matches a *user* definition of the name.  The definition/constructor
     * forms (`defprotocol`, `make-protocol`, `make-session`) are not
     * function-like and are left unconditional. */
    if (name == e->sym_defprotocol)   return elab_defprotocol(e, call);
    if (name == e->sym_make_protocol) return elab_make_protocol(e, call);
    if (name == e->sym_make_session)  return elab_session_make(e, call);
    if (!scope_lookup(e->scope, name)) {
        if (name == e->sym_send_to)       return elab_send_to(e, call);
        if (name == e->sym_recv_from)     return elab_recv_from(e, call);
        if (name == e->sym_recv_timeout_from) return elab_recv_timeout_from(e, call);
        if (name == e->sym_send)          return elab_session_send(e, call);
        if (name == e->sym_recv)          return elab_session_recv(e, call);
        /* SS5: close handles both TY_SESSION (binary) and TY_ROLE (multi-party) */
        if (name == e->sym_close)         return elab_session_close(e, call);
        if (name == e->sym_offer)         return elab_session_offer(e, call);
        if (name == e->sym_choose_left)   return elab_session_choose_left(e, call);
        if (name == e->sym_choose_right)  return elab_session_choose_right(e, call);
        if (name == e->sym_recv_timeout)  return elab_session_recv_timeout(e, call);
    }
    /* Phase R2: Panic */
    if (name == e->sym_panic) return elab_panic(e, call);
    if (name == e->sym_panic_with) return elab_panic_with(e, call);
    if (name == e->sym_catch_unwind) return elab_catch_unwind(e, call);
    if (name == e->sym_catch_panic_of) return elab_catch_panic_of(e, call);
    if (name == e->sym_panic_payload_type) return elab_panic_payload_type(e, call);
    if (name == e->sym_panic_payload_value) return elab_panic_payload_value(e, call);
    if (name == e->sym_panic_payload_file) return elab_panic_payload_file(e, call);
    if (name == e->sym_panic_payload_line) return elab_panic_payload_line(e, call);
    if (name == e->sym_panic_payload_downcast) return elab_panic_payload_downcast(e, call);
    /* Phase U3: Unsafe primitives - pointer operations */
    if (name == e->sym_ptr_deref)   return elab_ptr_deref(e, call);
    if (name == e->sym_ptr_write)  return elab_ptr_write(e, call);
    if (name == e->sym_ptr_add)     return elab_ptr_add(e, call);
    if (name == e->sym_ptr_sub)     return elab_ptr_sub(e, call);
    if (name == e->sym_ptr_nullq)   return elab_ptr_nullq(e, call);
    if (name == e->sym_ptr_of)      return elab_ptr_of(e, call);
    /* Phase U3: Unsafe primitives - type casting */
    if (name == e->sym_unsafe_cast) return elab_unsafe_cast(e, call);
    if (name == e->sym_reinterpret) return elab_reinterpret(e, call);
    if (name == e->sym_transmute)   return elab_transmute(e, call);
    /* Phase U3: Unsafe primitives - unchecked array ops */
    if (name == e->sym_array_get_unchecked)  return elab_array_get_unchecked(e, call);
    if (name == e->sym_array_set_unchecked)  return elab_array_set_unchecked(e, call);
    /* Phase U3: Unsafe primitives - raw memory */
    if (name == e->sym_raw_malloc)  return elab_raw_malloc(e, call);
    if (name == e->sym_raw_free)    return elab_raw_free(e, call);
    if (name == e->sym_raw_realloc) return elab_raw_realloc(e, call);
    if (name == e->sym_raw_memcpy)  return elab_raw_memcpy(e, call);
    if (name == e->sym_raw_memset)  return elab_raw_memset(e, call);
    /* Phase U3: Unsafe primitives - FFI */
    if (name == e->sym_c_call)      return elab_c_call(e, call);
    if (name == e->sym_call_ptr)    return elab_call_ptr(e, call);
    if (name == e->sym_callback_ptr) return elab_callback_ptr(e, call);
    if (name == e->sym_dlopen)      return elab_dlopen(e, call);
    if (name == e->sym_dlsym)       return elab_dlsym(e, call);
    if (name == e->sym_dlclose)     return elab_dlclose(e, call);
    /* Phase T19-B: thread-spawn (Send-safety check for cross-thread closures) */
    /* Only intercept when arg[1] is a literal (fn ...) form; if the user has  */
    /* defined their own thread-spawn function, let it fall through below.      */
    if (name == e->sym_thread_spawn &&
        call->as.list.len == 2 &&
        call->as.list.items[1]->tag == F_LIST &&
        call->as.list.items[1]->as.list.len >= 1 &&
        call->as.list.items[1]->as.list.items[0]->tag == F_SYM &&
        call->as.list.items[1]->as.list.items[0]->as.sym == e->sym_fn)
        return elab_thread_spawn(e, call);
    /* Phase T21-F: async/await sugar */
    if (name == e->sym_async && call->as.list.len == 2)
        return elab_async(e, call);
    if (name == e->sym_await && call->as.list.len == 2)
        return elab_await(e, call);
    /* Phase SEL1: fair multi-channel select */
    if (name == e->sym_select && call->as.list.len >= 2)
        return elab_select(e, call);
    /* Phase 20: Software Transactional Memory */
    if (name == e->sym_stm) return elab_stm(e, call);
    /* Ungated, like `stm` one line up.  The row used to gate on `len == 2`, so
     * every other arity fell out of this table and reported `unknown function
     * or operator 'atomically'` -- a wrong diagnostic for an arity mistake.
     * The body is variadic now, and a bare `(atomically)` gets a real arity
     * error from elab_atomically. */
    if (name == e->sym_atomically) return elab_atomically(e, call);
    if (name == e->sym_retry) return elab_retry(e, call);
    if (name == e->sym_check && call->as.list.len == 2)
        return elab_check(e, call);
    if (name == e->sym_or_else && call->as.list.len == 3)
        return elab_or_else(e, call);
    if (name == e->sym_tvar_new && call->as.list.len == 2)
        return elab_tvar_new(e, call);
    if (name == e->sym_tvar_read && call->as.list.len == 2)
        return elab_tvar_read(e, call);
    if (name == e->sym_tvar_write && call->as.list.len == 3)
        return elab_tvar_write(e, call);
    if (name == e->sym_tvar_modify && call->as.list.len == 3)
        return elab_tvar_modify(e, call);
    if (name == e->sym_tvar_swap && call->as.list.len == 3)
        return elab_tvar_swap(e, call);
    if (name == e->sym_tvar_cas && call->as.list.len == 4)
        return elab_tvar_cas(e, call);
    /* TVar operations with / syntax */
    if (name == e->sym_tvar && call->as.list.len >= 2) {
        Form *op = call->as.list.items[1];
        if (op->tag == F_SYM) {
            if (op->as.sym == e->sym_new && call->as.list.len == 3)
                return elab_tvar_new(e, call);
            if (op->as.sym == e->sym_read && call->as.list.len == 3)
                return elab_tvar_read(e, call);
            if (op->as.sym == e->sym_write && call->as.list.len == 4)
                return elab_tvar_write(e, call);
            if (op->as.sym == e->sym_modify && call->as.list.len == 4)
                return elab_tvar_modify(e, call);
            if (op->as.sym == e->sym_swap && call->as.list.len == 4)
                return elab_tvar_swap(e, call);
            if (op->as.sym == e->sym_cas && call->as.list.len == 5)
                return elab_tvar_cas(e, call);
        }
    }
    /* Phase R1: ? operator — lowers to early-return on err */
    if (name == e->sym_question) {
        return elab_question(e, call);
    }
    /* Phase 15: Method call syntax - (.method obj arg1 arg2) */
    if (name->len > 0 && name->name[0] == '.') {
        /* RM1 (bind chains): `do-m` expands to `(.bind m (fn [x] ...))`, so
         * this route needs the same closure-argument hoist the bare-name
         * dispatch route applies -- gated, as there, on a statically resolved
         * instance whose masks are the callee's own. */
        Expr *mc = elab_method_call(e, call);
        return (mc && call_dispatch_is_static(mc))
            ? hoist_borrowed_closure_args(e, mc, call->span) : mc;
    }
    /* Phase 6 */
    if (name == e->sym_defmacro) return elab_defmacro(e, call);
    /* Stage 2 (macro-system-direction-plan): procedural macros. */
    if (name == e->sym_defmacro_star) return elab_defmacro_star(e, call);
    if (name == e->sym_quote) {
        /* (quote x) -- mirrors F_QUOTE in elab_toplevel.c. Quoting a
         * bare symbol yields a :Sym literal so DSL helpers in defns
         * can construct AST nodes without the inner symbol being
         * TUR-E0003-resolved against the scope. */
        Form *quoted = call->as.list.items[1];
        if (quoted->tag == F_SYM) {
            Expr *out = expr_new(e->arena, EX_SYM_LIT, TYPE_SYM, call->span);
            out->as.sym_lit_.sym = quoted->as.sym;
            return out;
        }
        return elab_form(e, quoted);
    }
    if (name == e->sym_gensym)   return elab_gensym(e, call);
    if (name == e->sym_thread)    return elab_thread(e, call);
    if (name == e->sym_thread_last) return elab_thread_last(e, call);

    /* Phase 2 */
    if (name == e->sym_defn)    return elab_defn  (e, call);
    if (name == e->sym_fn)      return elab_fn    (e, call);
    if (name == e->sym_lambda)  return elab_fn    (e, call); /* λ aliases fn */
    if (name == e->sym_extern_c) return elab_extern_c(e, call);

    /* Phase P3: HAMT lowering - lower map function calls when first arg is persistent */
    if (name == e->sym_map_new || name == e->sym_assoc || name == e->sym_dissoc ||
        name == e->sym_map_get || name == e->sym_map_has || name == e->sym_map_count ||
        name == e->sym_map_merge) {
        return elab_lower_map_call(e, call, name);
    }

    /* TMS3 (typed-map-surface-plan): hamt-of is now the single typed Map
     * builder and dispatches string keys through Hash[cstr]/MapKey[cstr] by
     * content, so the historical (hamt-of "k" ...) -> smap-of rewrite is no
     * longer needed -- string and int keys share one lowering. */

    /* Phase 6: Check if it's a macro call */
    MacroDef *macro = elab_lookup_macro(e, name);
    if (macro) {
        /* macro-depth-guard-loses-race-with-asan-stack: the depth counter is a
         * proxy for stack headroom, and on an ASan-instrumented build the
         * redzone-inflated frames can exhaust the real stack before 256 levels
         * -- the process then aborts with a sanitizer stack-overflow report
         * instead of this diagnostic.  So measure the real thing too: once a
         * genuine macro recursion is under way (depth >= 16 -- a runaway hits
         * that instantly, and the floor keeps a legitimately shallow nest on a
         * deliberately tiny `ulimit -s` from tripping), nearly-exhausted
         * headroom raises the SAME diagnostic pair the counter does. */
        bool stack_low = e->macro_expand_depth >= 16 &&
                         e->macro_expand_depth < ELAB_MAX_MACRO_EXPANSION_DEPTH &&
                         tur_stack_nearly_exhausted();
        /* TUR_DEBUG_STACK_GUARD=1: trace what the guard sees (used to verify
         * the trigger on a platform where the race reproduces). */
        {
            static int dbg = -1;
            if (dbg < 0) dbg = getenv("TUR_DEBUG_STACK_GUARD") != NULL;
            if (dbg) {
                size_t t = 0, h = tur_stack_headroom(&t);
                fprintf(stderr, "[stack-guard] depth=%u headroom=%zu total=%zu low=%d\n",
                        e->macro_expand_depth, h, t, (int)stack_low);
            }
        }
        if (e->macro_expand_depth >= ELAB_MAX_MACRO_EXPANSION_DEPTH || stack_low) {
            diag_emit(DIAG_ERROR, call->span, "maximum macro expansion depth exceeded");
            /* The two ways a recursive macro's base case silently never fires,
             * both measured in practice (see docs/guides/macros-guide.md):
             * an empty `& ^syntax` rest is an EMPTY LIST, so `nil?` on it is
             * always false (`empty?` is the right predicate); and the
             * compile-time evaluator has no arithmetic, so a spliced
             * `~(- n 1)` recurses on the unevaluated FORM `(- n 1)`, which
             * never equals 0. */
            diag_emit(DIAG_NOTE, call->span,
                      "a recursive macro whose base case never fires is the "
                      "usual cause: test an empty rest with (empty? xs), not "
                      "(nil? xs), and note the compile-time evaluator has no "
                      "arithmetic -- drive recursion by argument list "
                      "structure, not by counting");
            if (stack_low)
                diag_emit(DIAG_NOTE, call->span,
                          "expansion stopped at depth %u (limit %u): the "
                          "compiler's C stack is nearly exhausted -- oversized "
                          "native frames (e.g. a sanitizer-instrumented debug "
                          "build) or a small stack limit reach the stack "
                          "before the depth limit",
                          e->macro_expand_depth,
                          (unsigned)ELAB_MAX_MACRO_EXPANSION_DEPTH);
            return NULL;
        }
        /* ambiguous-dispatch-error-quality: record the OUTERMOST macro call site
         * so a diagnostic raised inside the expansion (e.g. a derive-emitted
         * `.method` call with no matching instance) can point the user at where
         * they wrote the macro call rather than at stdlib/macros.tur. */
        if (e->macro_expand_depth == 0) e->macro_call_site_span = call->span;
        /* The same site, for the LSP (diag_set_expansion_site).  Saved and
         * restored rather than cleared: a macro-time evaluation runs a nested
         * elaborator whose depth starts again at 0. */
        const char *saved_site_macro = NULL;
        Span saved_site = SPAN_UNKNOWN;
        bool outermost = e->macro_expand_depth == 0;
        if (outermost)
            saved_site = diag_set_expansion_site(call->span, name->name,
                                                 &saved_site_macro);
        e->macro_expand_depth++;
        /* macro-expansion provenance: template spans survive expansion, so an
         * error inside generated code points at the DEFMACRO body -- useless
         * to a caller who did not write the macro.  Bracket the whole
         * expand+elaborate window with the shown-error serial and, on the
         * OUTERMOST frame only, append one note naming the call the user
         * actually wrote.  Inner frames stay silent so a nested-macro error
         * gets a single note at the user-visible call, not one per layer. */
        uint64_t mx_err_before = diag_error_serial();
        /* Expand the macro with arguments */
        /* Extract arguments (rest of list) */
        uint32_t n_args = call->as.list.len - 1;
        Form **args = (n_args == 0) ? NULL : (Form **)arena_alloc(e->arena, n_args * sizeof(Form *));
        for (uint32_t i = 0; i < n_args; i++) {
            args[i] = call->as.list.items[1 + i];
        }
        
        Form *expanded = elab_expand_macro(e, macro, args, n_args);
        if (!expanded) {
            if (e->macro_expand_depth == 1 &&
                diag_error_serial() != mx_err_before)
                diag_emit(DIAG_NOTE, call->span,
                          "in expansion of macro '%s' -- the diagnostics above "
                          "are inside code this call generated",
                          name->name);
            if (outermost)
                (void)diag_set_expansion_site(saved_site, saved_site_macro, NULL);
            e->macro_expand_depth--;
            return NULL;
        }

        /* `tur expand`: trace each expansion as it happens.  Nested
         * expansions print too (inner-first is elaboration order), each
         * labeled with the macro name and the call site's line:col.
         * Expansions inside the stdlib preload are noise, skip them. */
        if (g_dump_expansion && !e->in_stdlib_load) {
            Buf mb;
            buf_init(&mb);
            form_print(&mb, expanded);
            /* Basename only: a full path would embed the build environment
             * into output meant for golden-file comparison.  A recursive
             * macro's inner calls carry the macro-definition file's spans,
             * so the file name is what disambiguates them from user code. */
            const char *dump_path = diag_file_path(call->span.file_id);
            const char *dump_base = dump_path ? strrchr(dump_path, '/') : NULL;
            dump_base = dump_base ? dump_base + 1 : (dump_path ? dump_path : "?");
            printf(";; %s @ %s:%u:%u\n%.*s\n", name->name, dump_base,
                   call->span.line, call->span.col_start,
                   (int)mb.len, mb.data);
            buf_free(&mb);
        }

        /* Re-attribute a macro-emitted top-level `definstance` to the macro
         * CALL SITE rather than the macro-definition file.  The orphan-instance
         * check (TUR-E0013) and inst->origin_file_id key off the definstance
         * form's own span.file_id; a macro that constructs the instance (e.g.
         * derive-show / derive-show-string in stdlib/macros.tur) would otherwise
         * carry the macros.tur file id, so the user's own struct type -- whose
         * origin is the call-site file -- is not credited and a legitimate
         * `derive-show-string MyStruct ...` trips the orphan check.  Re-spanning
         * only the outermost definstance form makes ownership follow the call
         * site (where the user wrote the derive); inner subforms keep their
         * macro-body spans so diagnostics inside the expansion still point at
         * the macro.  Restricted to definstance so no other macro's diagnostics
         * move.  The expansion output is freshly arena-allocated, so mutating
         * its span is safe. */
        if (expanded->tag == F_LIST && expanded->as.list.len > 0 &&
            expanded->as.list.items[0]->tag == F_SYM &&
            strcmp(expanded->as.list.items[0]->as.sym->name, "definstance") == 0) {
            expanded->span = call->span;
        }

        /* Phase M4: Keep the expansion-module context active while elaborating
         * the expanded form so private helper macros from the same module are
         * visible when the expansion calls them (e.g. triple → helper-double).
         * Cross-module wrapper-macro bug fix: also push on the stack so the
         * visibility extends across nested wrapper-macro expansions. */
        const Symbol *saved_expansion = e->macro_expansion_module;
        e->macro_expansion_module = macro->defining_module_name;
        if (e->n_macro_expansion_stack >= e->cap_macro_expansion_stack) {
            uint32_t nc = e->cap_macro_expansion_stack ? e->cap_macro_expansion_stack * 2 : 8;
            e->macro_expansion_stack = (const Symbol **)realloc(
                e->macro_expansion_stack, nc * sizeof(const Symbol *));
            e->cap_macro_expansion_stack = nc;
        }
        e->macro_expansion_stack[e->n_macro_expansion_stack++] =
            macro->defining_module_name;
        /* refine: link the call form to the expansion elaboration is about to
         * walk, so the crossing path walk can traverse macro-GENERATED
         * guards/crossings (see rt_macro_expansion in elab_fns.c). */
        refine_note_macro_expansion(e, call, expanded);
        /* The expansion REPLACES this call, so it inherits its statement
         * position: a top-level macro emitting `(do (defn ..) (def ..))` --
         * the ECS defsystem shape -- is a top-level statement list, not an
         * expression.  Without this hand-off the expanded Forms are fresh
         * allocations unreachable from `e->toplevel_stmt` (still the macro
         * CALL), and every `def` they emit is rejected as expression-position.
         * Only re-point when this call IS the current statement; a macro
         * called from inside an expression stays inside one. */
        const Form *saved_tl_stmt = e->toplevel_stmt;
        if (e->toplevel_stmt == call) e->toplevel_stmt = expanded;
        Expr *out = elab_form(e, expanded);
        e->toplevel_stmt = saved_tl_stmt;
        e->macro_expansion_module = saved_expansion;
        if (e->n_macro_expansion_stack > 0) e->n_macro_expansion_stack--;
        if (e->macro_expand_depth == 1 && diag_error_serial() != mx_err_before)
            diag_emit(DIAG_NOTE, call->span,
                      "in expansion of macro '%s' -- the diagnostics above are "
                      "inside code this call generated",
                      name->name);
        if (outermost)
            (void)diag_set_expansion_site(saved_site, saved_site_macro, NULL);
        e->macro_expand_depth--;
        return out;
    }

    /* Phase 2: Check if it's a user-defined function call.
     * M1: Use elab_lookup_sym for visibility + qualified name resolution. */
    bool fn_qual_err = false;
    Binding *fn_binding = elab_lookup_sym(e, name, head->span, &fn_qual_err);
    if (!fn_binding && fn_qual_err) return NULL;
    /* compiled-closure-copies-a-captured-mut: a call through a `^mut` moved
     * into a shared cell calls what the cell holds. */
    if (fn_binding && fn_binding->cell_hidden_sym) {
        Form **items = (Form **)arena_alloc(e->arena,
                                            call->as.list.len * sizeof(Form *));
        items[0] = elab_mut_cell_read_form(e, fn_binding, head->span);
        for (uint32_t k = 1; k < call->as.list.len; k++)
            items[k] = call->as.list.items[k];
        return elab_form(e, form_list(e->arena, call->span, items,
                                      call->as.list.len));
    }

    /* constrained-generic-as-value (docs/archive/history/constrained-generic-as-value-
     * bakes-representative.md): a call through an immutable let-bound alias of a
     * global function -- `(let [g count-it] (g box))` -- was elaborated as an
     * indirect call through `g`, so the emit-side per-call-site generic-dict
     * specialization (which keys on a named callee) never fired and the base
     * clone baked the carrier representative instance (returning the wrong
     * answer, rc=0).  An alias `g.source_binding == count-it` makes `(g args)`
     * semantically identical to `(count-it args)` (the binding is immutable and
     * source_binding is only set for global TY_FN inits, elab_forms.c), so
     * resolve the call head to the global here: the direct-call path below then
     * attaches the same abi_bindings a direct `(count-it args)` call gets, and
     * the GDE machinery specializes to the receiver's real instance. */
    if (fn_binding && !fn_binding->is_global && !fn_binding->is_poly_fn &&
        fn_binding->type.kind == TY_FN && fn_binding->source_binding &&
        fn_binding->source_binding->is_global &&
        fn_binding->source_binding->type.kind == TY_FN) {
        fn_binding = fn_binding->source_binding;
    }

    /* saffron-lang-plan D7: a region bracket in a Saffron file.
     *
     * Placed AFTER the alias resolution just above, so `(let [g with-region]
     * (g thunk))` is judged by what `g` resolves to rather than by how the
     * call is spelled -- and, for the same reason, a local genuinely named
     * `with-region` that is NOT the stdlib defn is not caught.  Gated on
     * `is_global` for that second half.
     *
     * `bt-scope` is deliberately NOT here even though it opens a region too
     * (emit_binding_is_region_scope treats the two identically).  Its FEATURE
     * is the trail level -- mark and undo -- and that half works in Saffron
     * exactly as it does in Turmeric; only the region half degrades to
     * retire-don't-rewind.  Rejecting it would take a working feature away to
     * report a lost optimisation.  `with-region` has no other half: the region
     * IS the whole of it, so a Saffron `with-region` is precisely the silent
     * no-op D7 exists to refuse. */
    if (fn_binding && fn_binding->is_global && fn_binding->name &&
        fn_binding->name->name &&
        strcmp(fn_binding->name->name, "with-region") == 0 &&
        saffron_reject_static_only(e, call->span, "with-region",
                                   "the rewind is licensed by a static walk "
                                   "over the bracket's result type, and `any` "
                                   "clears nothing -- the generation would be "
                                   "retired, so the bracket costs a push/pop "
                                   "and reclaims nothing"))
        return NULL;

    /* RT1 (refinement-types-plan): a direct call to a user function is a
     * crossing into whatever refinements its parameters declare.  Record it
     * here -- one place, keyed on the source form, before any of the dozen
     * downstream call-construction paths -- and resolve it after the whole
     * unit is elaborated, when every callee's predicates are known regardless
     * of definition order.  A no-op for a callee with no refined parameters. */
    if (fn_binding && fn_binding->type.kind == TY_FN)
        (void)refine_note_call_site(e, fn_binding, call, 1);

    /* Phase RT: return-type-directed dispatch for a typeclass method whose
     * dispatch variable appears only in its return type (e.g. (default-of),
     * (schema-of)).  Such methods cannot be resolved from arguments; the
     * instance is selected from the expected-type channel.  These methods did
     * not exist before tyvar-return parsing was added, so intercepting them
     * here cannot regress existing programs. */
    /* Gate on `!fn_binding` so a user defn or local binding of the same name
     * wins over a return-directed typeclass method -- mirroring the GHE1
     * bare-method gating below.  Without this, migrating a class method to a
     * return-directed by-value sig (e.g. Applicative `pure : (f a)`) would
     * hijack an unrelated user `(defn pure ...)` (parsec-tutorial reimplements
     * its own `pure`), routing the call to the wrong (carrier-representative)
     * instance. */
    if (!fn_binding) {
        bool rt_handled = false;
        Expr *rt = elab_try_return_dispatch(e, call, name, &rt_handled);
        if (rt_handled) return rt;
    }

    /* Phase GHE1: bare-name typeclass method dispatch.  A head symbol that
     * names a registered typeclass method but resolves to no binding (no user
     * defn and no local shadow) is routed to argument-type dispatch, exactly as
     * the dotted (.method ...) form would be: the first argument is the
     * receiver, and its static type selects the instance.  This lets bare
     * (hash x), (eq? a b), (show v) pick the right instance instead of falling
     * through to the eval-mode native fallback (which types them :int).
     *
     * Gated on `!fn_binding` so a user defn or local binding of the same name
     * normally wins, and on class membership so a program that never declared
     * the class is never intercepted -- a genuinely-unbound symbol still flows
     * to its original unbound-symbol / eval-native handling.  Return-only
     * dispatch methods were already handled above; argument-dispatched methods
     * always carry their dispatch type variable in the first parameter for the
     * stdlib classes (Eq/Hash/Show/Num/Functor/...).
     *
     * Namespace separation (fix (1) of
     * docs/archive/history/typeclass-methods-share-value-namespace-with-defns.md):
     * when a free `defn` *and* a user-defined typeclass method share the name,
     * the `!fn_binding` gate alone would make the defn shadow the method at
     * every bare call site.  Instead, when an instance of the user class
     * matches the receiver's static type we prefer dispatch (`prefer_method_
     * dispatch`), so a class method and a same-named free helper can coexist in
     * one module.  If no instance matches, the free defn still wins.  Stdlib
     * methods are excluded by elab_user_method_instance_matches, preserving the
     * documented "user defn overrides a stdlib method" pattern. */
    bool prefer_method_dispatch = false;
    if (fn_binding && call->as.list.len >= 2 &&
        elab_name_is_typeclass_method(e, name)) {
        Expr *recv0 = elab_form(e, call->as.list.items[1]);
        if (recv0 && elab_user_method_instance_matches(e, name, &recv0->type))
            prefer_method_dispatch = true;
    }
    if ((!fn_binding || prefer_method_dispatch) && call->as.list.len >= 2 &&
        elab_name_is_typeclass_method(e, name)) {
        char dotbuf[160];
        int dotlen = snprintf(dotbuf, sizeof(dotbuf), ".%s", name->name);
        if (dotlen > 0 && (size_t)dotlen < sizeof(dotbuf)) {
            const Symbol *dot_sym =
                symtab_intern(e->st, strslice(dotbuf, (uint32_t)dotlen));
            uint32_t n_items = call->as.list.len;
            Form **items = (Form **)arena_alloc(e->arena, n_items * sizeof(Form *));
            items[0] = form_sym(e->arena, head->span, dot_sym);
            for (uint32_t i = 1; i < n_items; i++) items[i] = call->as.list.items[i];
            Form *dotcall = form_list(e->arena, call->span, items, n_items);
            /* A statically resolved dispatch carries the instance method as
             * fn_binding, so its inferred non-retaining mask makes an inline
             * continuation's env freeable at this scope -- the same hoist a
             * defn call gets. */
            Expr *mc = elab_method_call(e, dotcall);
            return (mc && call_dispatch_is_static(mc))
                ? hoist_borrowed_closure_args(e, mc, call->span) : mc;
        }
    }

    /* CONV-S4 (struct/ADT convergence): keyword construction for record-style
     * variants -- `(Circle :radius 2.0)` reorders to the positional call
     * `(Circle 2.0)`.  Mirrors make-struct keyword construction (KW-V0).  Only
     * fires for a record-style ctor whose first argument is a keyword; the
     * rewritten positional form then flows through the normal ctor paths below. */
    if (fn_binding && call->as.list.len >= 2 &&
        (fn_binding->type.kind == TY_ADT ||
         (fn_binding->type.kind == TY_FN &&
          fn_binding->type.as.fn.result_kind == TY_ADT))) {
        CtorDef *kwctor = elab_lookup_ctor(e, name);
        if (kwctor && kwctor->is_record) {
            /* A leading keyword means KEYWORD CONSTRUCTION only when it
             * NAMES A FIELD.  A record with a `Sym` field takes keywords as
             * VALUES -- `(make-struct P :kw)` -- and reading that as a field
             * name made such a record unconstructable ("keyword construction
             * needs :field value pairs"), which is how the saffron fuzzer's
             * Sym probe still failed after `Sym` became a legal field type.
             *
             * A keyword that DOES name a field keeps the keyword reading, so
             * `(Circle :radius 2.0)` is unchanged; a typo'd field name now
             * lands on the ordinary positional arity/type error instead of
             * the keyword one, which is a worse message but never a wrong
             * program.  The one genuinely ambiguous shape is a keyword value
             * that happens to equal one of the record's own field names --
             * `(make-struct P :fld)` where `fld` is the field -- which still
             * reads as keyword construction; bind the symbol first to say the
             * other thing. */
            bool first_kw = call->as.list.items[1]->tag == F_KEYWORD;
            bool any_kw = false;
            for (uint32_t a = 1; a < call->as.list.len; a++)
                if (call->as.list.items[a]->tag == F_KEYWORD) { any_kw = true; break; }
            /* ...unless the leading keyword is a Sym VALUE rather than a
             * field name.  A record whose first field is `Sym` is constructed
             * with a keyword in that position -- `(make-struct P :kw)` -- and
             * reading it as a field name made such a record unconstructable
             * ("keyword construction needs :field value pairs").  That is how
             * the saffron fuzzer's Sym probe still failed after `Sym` became a
             * legal field type: the field type was only half of what it needed.
             *
             * `(Circle :diameter 2.0)` (a typo) and `(make-struct Q :label
             * 9.75)` (a Sym value then a float) are the SAME SHAPE
             * syntactically, so the count alone cannot separate them -- but
             * the declared TYPES can.  Positional is chosen only when all
             * three hold: the keyword names no field, the argument count
             * equals the field count, and the first field is `Sym`-typed.
             * `(Circle :diameter 2.0)` has one field and two arguments, so it
             * stays keyword construction and keeps its precise
             * "unknown field 'diameter' on variant 'Circle'", which the
             * errors/conv-kw-record-variant-unknown-field family pins.
             *
             * The residual is a typo'd field name on a Sym-first record with a
             * matching count -- `(make-struct Q :labl 9.75)` -- which builds
             * with `tag = :labl`.  That is not really a residual: `:labl` is a
             * perfectly good Sym value, and nothing in the program says the
             * author meant a field name. */
            if (first_kw && kwctor->n_fields > 0 &&
                call->as.list.len - 1 == kwctor->n_fields &&
                kwctor->fields[0].kind == TY_SYM) {
                const char *kwn = call->as.list.items[1]->as.sym->name;
                bool names_field = false;
                for (uint32_t fi = 0; fi < kwctor->n_fields && !names_field; fi++)
                    if (kwctor->fields[fi].name &&
                        strcmp(kwctor->fields[fi].name, kwn) == 0)
                        names_field = true;
                if (!names_field) { first_kw = false; any_kw = false; }
            }
            /* Positional argument(s) followed by a keyword -- a mix.  (The
             * keyword-FIRST mix, `(P :x 1 2)`, is caught inside the pair loop
             * below.)  The same diagnostic the make-struct path emits, so a
             * `defstruct` lowered to a record ADT keeps the precise error
             * instead of falling through to a confusing "not callable". */
            if (any_kw && !first_kw) {
                char surf[160];
                conv_surface_phrase(kwctor->adt, kwctor, surf, sizeof(surf));
                diag_emit(DIAG_ERROR, call->span,
                          "TUR-E0299: %s construction: cannot mix positional "
                          "and keyword arguments", surf);
                return NULL;
            }
            if (first_kw) {
                uint32_t n_args = call->as.list.len - 1;
                if (n_args % 2 != 0) {
                    diag_emit(DIAG_ERROR, call->span,
                              "constructor '%s': keyword construction needs "
                              ":field value pairs", kwctor->name);
                    return NULL;
                }
                uint32_t n_pairs = n_args / 2;
                Form **pos_items = (Form **)arena_alloc(e->arena,
                                       (kwctor->n_fields + 1) * sizeof(Form *));
                pos_items[0] = call->as.list.items[0];
                for (uint32_t fi = 0; fi < kwctor->n_fields; fi++) pos_items[fi + 1] = NULL;
                /* Validate each pair (unknown / duplicate) FIRST -- before any
                 * arity check -- so a duplicate or unknown field is named
                 * precisely rather than masked by the off-by-one count it
                 * produces (`:age :age` would otherwise read as "got 3, want 2"). */
                for (uint32_t pi = 0; pi < n_pairs; pi++) {
                    Form *kw = call->as.list.items[1 + pi * 2];
                    Form *val = call->as.list.items[2 + pi * 2];
                    if (kw->tag != F_KEYWORD) {
                        char surf[160];
                        conv_surface_phrase(kwctor->adt, kwctor, surf, sizeof(surf));
                        diag_emit(DIAG_ERROR, kw->span,
                                  "TUR-E0299: %s construction: cannot mix "
                                  "positional and keyword arguments", surf);
                        return NULL;
                    }
                    int fidx = -1;
                    for (uint32_t fi = 0; fi < kwctor->n_fields; fi++) {
                        if (kwctor->fields[fi].name &&
                            strcmp(kwctor->fields[fi].name, kw->as.sym->name) == 0) {
                            fidx = (int)fi; break;
                        }
                    }
                    if (fidx < 0) {
                        char surf[160];
                        conv_surface_phrase(kwctor->adt, kwctor, surf, sizeof(surf));
                        diag_emit(DIAG_ERROR, kw->span,
                                  "TUR-E0294: unknown field '%s' on %s",
                                  kw->as.sym->name, surf);
                        return NULL;
                    }
                    if (pos_items[fidx + 1]) {
                        char surf[160];
                        conv_surface_phrase(kwctor->adt, kwctor, surf, sizeof(surf));
                        diag_emit(DIAG_ERROR, kw->span,
                                  "TUR-E0293: duplicate field '%s' in %s construction",
                                  kw->as.sym->name, surf);
                        return NULL;
                    }
                    pos_items[fidx + 1] = val;
                }
                /* Any field left unset is missing (this also subsumes the
                 * too-few-pairs case). */
                for (uint32_t fi = 0; fi < kwctor->n_fields; fi++) {
                    if (!pos_items[fi + 1]) {
                        char surf[160];
                        conv_surface_phrase(kwctor->adt, kwctor, surf, sizeof(surf));
                        diag_emit(DIAG_ERROR, call->span,
                                  "TUR-E0292: missing field '%s' in %s",
                                  kwctor->fields[fi].name, surf);
                        return NULL;
                    }
                }
                call = form_list(e->arena, call->span, pos_items, kwctor->n_fields + 1);
            }
        }
    }

    /* Phase G0: constructor call — (Ctor) or (Ctor :T1 ...) */
    if (fn_binding && fn_binding->type.kind == TY_ADT) {
        /* 0-arg constructor */
        AdtDef *adt = fn_binding->type.as.adt_.def;
        CtorDef *ctor = NULL;
        for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
            if (strcmp(adt->ctors[ci]->name, name->name) == 0) {
                ctor = adt->ctors[ci];
                break;
            }
        }
        if (ctor && ctor->n_fields == 0) {
            uint32_t n_args_given = call->as.list.len - 1;
            if (n_args_given != 0) {
                diag_emit(DIAG_ERROR, call->span,
                          "constructor '%s' takes 0 arguments, got %u",
                          name->name, n_args_given);
                return NULL;
            }
            /* Parametric 0-arg constructor result-type inference: a nullary
             * constructor of a parametric ADT (e.g. `(PFail)` from
             * `(defdata PRes [a] (PFail) (POK a int))`) carries no field
             * arguments to bind the type parameters from, so it defaults to
             * the bare TY_ADT.  When the enclosing expected type is a
             * concrete TY_APP over the same ADT (`(PRes Expr)` from the
             * function return type, a match arm's peer type, an ascription,
             * etc.), use it as the result type so bare `(PFail)` unifies
             * with concrete `(POK v rest)` peers.  See
             * docs/archive/history/defdata-parametric-inference-and-elab-match-segv.md. */
            Type result_type = fn_binding->type;
            bool result_pinned = false;
            if (ctor->adt->n_type_params > 0 && e->expected_type &&
                e->expected_type->kind == TY_APP) {
                AdtDef *exp_def = type_adt_app_def(e->expected_type);
                if (exp_def == ctor->adt) {
                    result_type = *e->expected_type;
                    result_pinned = true;
                }
            }
            /* saffron-dynamic-surface-pass M3: a NULLARY constructor of a
             * parametric ADT has no type-variable field for the Saffron ctor
             * widen above to ascribe, so `(Nothing)` was built at the bare
             * ADT while `(Just 1)` was `(Opt any)`: `(is? (Nothing) (Opt any))`
             * was false compiled and a `match` on an `any` holding it panicked
             * `different instantiation of Opt`.  The dialect's rule is one
             * sentence -- an undetermined type argument is `any` -- so build
             * it at the all-`any` instantiation, exactly as the instance
             * registry keys a parametric receiver. */
            if (!result_pinned && ctor->adt->n_type_params > 0 &&
                lang_span_is_dynamic(call->span)) {
                Type any_t = type_from_kind(TY_ANY);
                Type app = type_adt(ctor->adt);
                app.hkt_kind = kind_for_arity(ctor->adt->n_type_params);
                for (uint8_t pi = 0; pi < ctor->adt->n_type_params; pi++)
                    app = type_app(e->arena, app, any_t, (Span){0});
                result_type = app;
            }
            Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
            out->as.call_.fn_binding = fn_binding;
            out->as.call_.args = NULL;
            out->as.call_.n_args = 0;
            out->as.call_.fn_expr = NULL;
            out->as.call_.dict_arg = NULL;
            /* SZ8: a nullary sized-GADT constructor seeds the size index from its
             * (constant) return-type template, e.g. SVNil : (SizedVec (Static 0)). */
            out->as.call_.ctor = ctor;
            SizeTerm *inferred = sz8_infer_ctor_size_index(e, ctor, out);
            out->as.call_.size_index = inferred;
            sz8_dump_ctor_size(e, ctor, inferred);
            out->as.call_.gadt_form = gadt_infer_ctor_form(e, ctor, out, call, NULL);
            return out;
        }
    }

    /* Phase G0: N-arg constructor call — result type needs ADT def pointer */
    if (fn_binding && fn_binding->type.kind == TY_FN &&
        fn_binding->type.as.fn.result_kind == TY_ADT) {
        /* Look up the constructor to find its AdtDef */
        CtorDef *ctor = elab_lookup_ctor(e, name);
        if (ctor) {
            /* Use elab_call_fn but fix up the result type after */
            Expr *call_expr = elab_call_fn(e, call, fn_binding);
            /* struct-curry-ctor: an UNDER-APPLIED constructor call partial-applies
             * into a closure (elab_call_fn -> elab_partial_apply, result type
             * TY_PTR_VOID) that completes to the ADT.  The result-type patch and
             * the per-field consistency/size-index fixups below assume a saturated
             * ctor call; running them on a closure would stamp the partial app as
             * the full ADT (so `((Person "Ada") 36)` saw `(Person "Ada")` as a
             * non-callable Person).  Detect the partial app by arity and return
             * the closure untouched. */
            if (call_expr &&
                (call->as.list.len - 1u) < ctor->n_fields &&
                call_expr->type.kind != TY_ADT && call_expr->type.kind != TY_APP) {
                return call_expr;
            }
            if (call_expr) {
                /* Patch result type with proper AdtDef pointer */
                call_expr->type = type_adt(ctor->adt);

                /* SZ8: record the CtorDef and infer the type-level size index of
                 * this constructed value (sized GADTs only; erased in codegen). */
                call_expr->as.call_.ctor = ctor;
                SizeTerm *inferred = sz8_infer_ctor_size_index(e, ctor, call_expr);
                call_expr->as.call_.size_index = inferred;
                sz8_dump_ctor_size(e, ctor, inferred);
                /* gadt-length-index-not-enforced: the value's phantom index. */
                if (ctor->adt->is_gadt && call_expr->kind == EX_CALL) {
                    bool gmis = false;
                    call_expr->as.call_.gadt_form =
                        gadt_infer_ctor_form(e, ctor, call_expr, call, &gmis);
                    if (gmis) return NULL;
                }

                /* TP5: intra-constructor type-arg consistency check.
                 * For each field whose full_type is a named TY_TYVAR, record
                 * the concrete argument type.  If a later field binds the same
                 * param to a different type, emit a diagnostic. */
                struct { const char *name; Type type; } param_bindings[8];
                uint8_t  n_bound = 0;
                uint32_t n_call_args = call_expr->as.call_.n_args;
                for (uint32_t fi = 0; fi < ctor->n_fields && fi < n_call_args; fi++) {
                    const Type *ft = ctor->fields[fi].full_type;
                    if (!ft || ft->kind != TY_TYVAR || !ft->as.tyvar_.name) continue;
                    const char *pname = ft->as.tyvar_.name;
                    /* TS4P1: If the argument was wrapped in EX_REINTERPRET (boxing a
                     * concrete type like float into int64_t for the TY_INT carrier),
                     * use the source type (before the reinterpret) as the concrete type.
                     * This ensures we bind e.g. `a -> float` instead of `a -> int`. */
                    const Expr *arg_expr = call_expr->as.call_.args[fi];
                    while (arg_expr && arg_expr->kind == EX_REINTERPRET &&
                           arg_expr->as.reinterpret_.target_kind == TY_INT &&
                           arg_expr->as.reinterpret_.expr) {
                        arg_expr = arg_expr->as.reinterpret_.expr;
                    }
                    Type concrete = arg_expr ? arg_expr->type : call_expr->as.call_.args[fi]->type;
                    bool found = false;
                    for (uint8_t bi = 0; bi < n_bound; bi++) {
                        if (strcmp(param_bindings[bi].name, pname) == 0) {
                            Type prev = param_bindings[bi].type;
                            bool mismatch = (prev.kind != concrete.kind);
                            if (!mismatch && concrete.kind == TY_ADT)
                                mismatch = (prev.as.adt_.def != concrete.as.adt_.def);
                            if (mismatch) {
                                diag_emit(DIAG_ERROR,
                                    call->as.list.items[1 + fi]->span,
                                    "constructor '%s': type parameter '%s' was bound to "
                                    "'%s' by an earlier field but argument %u has type '%s'",
                                    ctor->name, pname,
                                    type_name(prev),
                                    fi, type_name(concrete));
                            }
                            found = true;
                            break;
                        }
                    }
                    if (!found && n_bound < 8) {
                        param_bindings[n_bound].name = pname;
                        param_bindings[n_bound].type = concrete;
                        n_bound++;
                    }
                }

                /* hkt-cata-function-carrier: a concrete (statically TY_FN)
                 * function value stored into a PARAMETRIC ADT field (one
                 * declared as a bare type variable, e.g. `a` in
                 * `(defdata ExprF [a] (AddF a a))`) must be boxed into a uniform
                 * fat closure.  The constructor lowers the field to the int64
                 * carrier, so the TY_TYVAR auto-shim in the arg loop does not
                 * fire here -- without this a captureless lambda stored in
                 * `(AddF f g)` stays a thin fn pointer, and the match-arm
                 * extraction (marked is_fat) would fat-dispatch a thin pointer,
                 * jumping into code as if it were an env block -> SIGSEGV.
                 * A *carrier-erased* fn value -- a fold's `(g x)` recursion
                 * result inside a generic `fmap`/`cata` body, elaborated as the
                 * int64 carrier (TY_INT) because the carrier type B is still an
                 * unbound tyvar -- is NOT statically TY_FN, so the TY_FN gate
                 * skips it: it is already a fat box (the algebra returns
                 * closures) and must not be double-boxed.  A capturing closure
                 * value (TY_PTR_VOID) and an already-boxed TY_FN are likewise
                 * left untouched.  Mirrors the ^fat auto-shim arity bound (<=5). */
                for (uint32_t fi = 0; fi < ctor->n_fields && fi < n_call_args; fi++) {
                    const Type *ft = ctor->fields[fi].full_type;
                    /* A parametric (TY_TYVAR) field, or -- capturing-closure-in-
                     * struct-field-segv -- a concrete boxed `(fn ...)` field, both
                     * carry the fat representation, so a bare/thin fn argument must
                     * be shimmed into a fat `{thunk, env}` handle (EX_FN_TO_FAT).
                     * A capturing-closure value (TY_PTR_VOID) and an already-boxed
                     * TY_FN are left untouched -- already fat. */
                    bool fat_field = ft && (ft->kind == TY_TYVAR ||
                                            (ft->kind == TY_FN && ft->as.fn.boxed));
                    if (!fat_field) continue;
                    Expr *fa = call_expr->as.call_.args[fi];
                    if (!fa || fa->type.kind != TY_FN || fa->type.as.fn.boxed)
                        continue;
                    /* fn-value-fat-normalization increment 2: a NORMALIZED fn
                     * param already holds a fat handle even though its declared
                     * type is not marked `boxed` (normalization is a property of
                     * the type, not a flag on it).  Storing it into a fat field
                     * must pass through -- the `boxed` test above cannot see it,
                     * and re-boxing makes the field's slot 0 the outer shim, so
                     * the field call runs the handle as code.  This is the
                     * struct-store twin of the A#1 forwarding guard; it is what
                     * `stdlib/lens.tur`'s `(lens get put)` hits once `get`/`put`
                     * (tyvar signatures) are normalized. */
                    if (fa->kind == EX_VAR && fa->as.var.binding &&
                        fa->as.var.binding->is_param &&
                        (fa->as.var.binding->is_fat ||
                         fn_param_type_is_fat_normalized(&fa->as.var.binding->type)))
                        continue;
                    uint32_t inner_arity = fa->type.as.fn.arity;
                    /* Arity 0 admitted alongside the field-boxing change in
                     * resolve_ctor_field: a thunk field is boxed now, so a
                     * thin nullary fn stored into it needs the same shim. */
                    if (inner_arity > TUR_FAT_SHIM_MAX_ARITY) continue;
                    Type *bt = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *bt = fa->type;
                    bt->as.fn.boxed = true;
                    Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, *bt, fa->span);
                    shim->as.fn_to_fat_.inner = fa;
                    /* fnsan-concrete-field-sink: a concrete `(fn ...)` field is
                     * read at exactly its declared types (TUR_APPLY<N>_T), as a
                     * concrete `^fat` parameter is -- hand the box the same
                     * sink type, so a small by-value aggregate result gets the
                     * typed shim rather than the word one. */
                    if (ft->kind == TY_FN && !call_type_has_named_tyvar(ft))
                        shim->as.fn_to_fat_.sink_fn_type = ft;
                    call_expr->as.call_.args[fi] = shim;
                }

                /* closure-in-defdata-field, the residue: a field declared as
                 * bare `:fn` (no signature -- full_type NULL) or as a >4-arity
                 * `(fn ...)` stays on the THIN calling convention; there is no
                 * signature (or no TUR_APPLY<N>_T) to box it with.  A
                 * CAPTURING closure stored into such a field is a fat env
                 * block that every force site will jump into as code --
                 * a guaranteed runtime SIGSEGV with no diagnostic anywhere.
                 * Reject the store instead.  Captureless lambdas lift to bare
                 * function pointers and stay correct on the thin path, and
                 * every in-tree `:fn` field stores exactly those, so this
                 * rejects nothing that works today. */
                for (uint32_t fi = 0; fi < ctor->n_fields && fi < n_call_args; fi++) {
                    if (ctor->fields[fi].kind != TY_FN) continue;
                    const Type *ft = ctor->fields[fi].full_type;
                    bool thin_fn_field =
                        !ft || (ft->kind == TY_FN && !ft->as.fn.boxed);
                    if (!thin_fn_field) continue;
                    const Expr *fa = call_expr->as.call_.args[fi];
                    while (fa && fa->kind == EX_ASCRIBE) fa = fa->as.ascribe_.inner;
                    /* The same field, the other way to miscompile it: a bare
                     * `fn` field carries no signature, so every call through
                     * it spells an int64 result -- a function with a float
                     * argument or result stored here came back as its bits
                     * (`(.app b 3.25)` of a doubling fn printed
                     * 4619004367821864960).  The `:fn` parameter carrier has
                     * rejected exactly this since Phase CCL; the field form of
                     * the same erasure never did. */
                    if (!ft && fa && fn_sig_has_float_class(&fa->type)) {
                        diag_emit(DIAG_ERROR, fa->span,
                                  "constructor '%s': a function with a "
                                  "floating-point argument or result cannot be "
                                  "stored in field %u, which is declared bare "
                                  "`fn` (no signature)\n"
                                  "  = note: every call through a signature-less "
                                  "`fn` field uses the int64 register class, so "
                                  "the float would be a silent miscompile\n"
                                  "  = help: declare the field's function type, "
                                  "e.g. (fn [float] float)",
                                  ctor->name, fi);
                        return NULL;
                    }
                    if (fa && fa->kind == EX_CLOSURE && fa->as.closure_.closure &&
                        fa->as.closure_.closure->n_captures > 0) {
                        diag_emit(DIAG_ERROR, fa->span,
                                  "constructor '%s': a capturing closure cannot be "
                                  "stored in field %u, whose type ('%s') uses the "
                                  "thin function-pointer representation\n"
                                  "  = note: the closure's environment would be "
                                  "called as code at every use -- a runtime crash\n"
                                  "  = help: declare the field with a full "
                                  "signature of arity <= 4, e.g. (fn [int] int), "
                                  "which stores closures correctly; or wrap the "
                                  "closure in an opaque carrier "
                                  "((defopaque Th :ptr<void>)) as stdlib/logic.tur's "
                                  "Goal does",
                                  ctor->name, fi,
                                  ft ? type_name(*ft) : "fn");
                    }
                }

                /* closure-drop-glue S2 (Model U): storing a CAPTURING closure
                 * VARIABLE into an owning (boxed) fn-field MOVES it -- the
                 * struct's drop glue frees that heap env, so a second use (another
                 * store, or a call) must be a use-after-consume error rather than
                 * a silent double-free (confirmed with valgrind).  A thin fn is
                 * re-shimmed to a FRESH box per store, and an inline closure has no
                 * source binding, so both are already uniquely owned and NOT
                 * consumed here -- only a variable holding a live capturing-closure
                 * handle (TY_PTR_VOID, or an already-boxed TY_FN) is moved. */
                for (uint32_t fi = 0; fi < ctor->n_fields && fi < n_call_args; fi++) {
                    const Type *ft = ctor->fields[fi].full_type;
                    if (!ft || ft->kind != TY_FN || !ft->as.fn.boxed) continue;
                    Expr *fa = call_expr->as.call_.args[fi];
                    while (fa && fa->kind == EX_ASCRIBE) fa = fa->as.ascribe_.inner;
                    if (fa && fa->kind == EX_VAR && fa->as.var.binding &&
                        (fa->type.kind == TY_PTR_VOID ||
                         (fa->type.kind == TY_FN && fa->type.as.fn.boxed)))
                        binding_mark_moved(fa->as.var.binding, fa->span);
                }

                /* TS4P1 / nested-carrier-match: Build TY_APP result type for
                 * per-use-site ADT monomorphisation.  Ground EVERY type param by
                 * unifying each field's declared full_type against the argument's
                 * actual type, descending into TY_APP / TY_FN fields -- not just
                 * the bare-tyvar fields the TP5 consistency loop above captured.
                 * Without this, a param that appears only inside a parametric
                 * field (e.g. `N`'s sole field `(Pair2 a a)` in
                 * `(defdata Nest [a] (N (Pair2 a a)))`) never binds, n_bound is
                 * 0, and the result stays a bare `Nest` -- so a nested `match`
                 * can't thread the concrete element type into the inner bindings.
                 * Collecting through the app makes `(N (MkPair2 3 4))` infer
                 * `(Nest int)`. */
                if (ctor->adt->n_type_params > 0 &&
                    !ctor->adt->is_gadt &&
                    ctor->adt->n_type_params <= 8) {
                    uint8_t ntp = ctor->adt->n_type_params;
                    Type targs[8];
                    bool have[8];
                    memset(targs, 0, sizeof(targs));
                    memset(have, 0, sizeof(have));
                    for (uint32_t fi = 0;
                         fi < ctor->n_fields && fi < n_call_args; fi++) {
                        const Type *ft = ctor->fields[fi].full_type;
                        if (!ft) continue;
                        /* TS4P1: unwrap an EX_REINTERPRET int64-carrier box so a
                         * float payload infers as float, not the int carrier. */
                        const Expr *arg_expr = call_expr->as.call_.args[fi];
                        while (arg_expr && arg_expr->kind == EX_REINTERPRET &&
                               arg_expr->as.reinterpret_.target_kind == TY_INT &&
                               arg_expr->as.reinterpret_.expr) {
                            arg_expr = arg_expr->as.reinterpret_.expr;
                        }
                        Type actual = arg_expr
                            ? arg_expr->type
                            : call_expr->as.call_.args[fi]->type;
                        adt_field_collect_type_args(ctor->adt->type_params, ntp,
                                                    ft, actual, targs, have);
                    }
                    bool all_bound = true;
                    for (uint8_t pi = 0; pi < ntp; pi++) {
                        if (!have[pi] || targs[pi].kind == TY_TYVAR) {
                            all_bound = false;
                            break;
                        }
                    }
                    /* When the arg binding collapsed to a bare TY_TYVAR (a
                     * polymorphic value passed through a tyvar field, e.g.
                     * `v : A` in a combinator body), consult the enclosing
                     * expected type -- if it names the same ADT def, adopt
                     * its concrete args so the ctor result becomes a real
                     * TY_APP instead of the bare TY_ADT fallback.  Gated on
                     * a matching expected type so it never fires in the
                     * classic Functor / cata paths where fmap's arms build
                     * bare-ADT results uniformly and no outer expected type
                     * is present. */
                    /* An expected type that is itself OPEN -- a sibling match
                     * arm's `(Either int R)` -- pins nothing: it is another
                     * constructor's partial answer, not an annotation.  Filling
                     * `(Right d)` out from it made `(Either int int)`, a by-value
                     * monomorph, beside a sibling that is still the carrier. */
                    if (!all_bound && e->expected_type &&
                        e->expected_type->kind == TY_APP &&
                        type_adt_app_def(e->expected_type) == ctor->adt &&
                        !type_has_open_slot(e->expected_type)) {
                        Type ex_args[8];
                        AdtDef *ex_def = NULL;
                        uint8_t ex_n = 0;
                        if (type_extract_adt_app(e->expected_type, &ex_def,
                                                 ex_args, &ex_n) &&
                            ex_def == ctor->adt && ex_n == ntp) {
                            bool recovered = true;
                            /* A failed rescue leaves the arguments as the
                             * fields gave them: the open-parameter typing
                             * below reads them, and a variable adopted from
                             * a CALLEE's parameter type -- `(Err 3)` passed
                             * to `pick-ok [A B]` meets `(Result A B)` --
                             * would leak that callee's `A` into the value. */
                            Type saved_targs[8];
                            bool saved_have[8];
                            memcpy(saved_targs, targs, sizeof(saved_targs));
                            memcpy(saved_have, have, sizeof(saved_have));
                            for (uint8_t pi = 0; pi < ntp; pi++) {
                                if (!have[pi]) {
                                    targs[pi] = ex_args[pi];
                                    have[pi] = true;
                                } else if (targs[pi].kind == TY_TYVAR) {
                                    targs[pi] = ex_args[pi];
                                }
                                if (targs[pi].kind == TY_TYVAR) {
                                    recovered = false;
                                }
                            }
                            if (recovered) {
                                all_bound = true;
                            } else {
                                memcpy(targs, saved_targs, sizeof(saved_targs));
                                memcpy(have, saved_have, sizeof(saved_have));
                            }
                        }
                    }
                    /* fmap-over-underdetermined-constructor-is-a-defless-shell:
                     * a constructor that fixes SOME parameters but not all --
                     * `(Ok 7.1)` fixes A, and nothing names B -- used to fall
                     * back to the bare ADT, which says nothing about A.  A
                     * generic consumer then bound nothing from it:
                     * `(ok-val (Ok 7.1))` returned the float's bits as an int,
                     * and `fmap` had no application chain to ground its result
                     * against.  Type it the way the generic `ok` constructor
                     * function already is: the application, with each open
                     * parameter left as the ADT's own type variable --
                     * `(Result float B)`, the same type `(ok 7.1)` has.  A
                     * Saffron file's undetermined argument is `any`, as its
                     * nullary constructors already are.  Only when a field
                     * fixed a parameter to a concrete type: with nothing
                     * concrete there is nothing to say, and the bare ADT stays. */
                    /* generic-ctor-over-sig-tyvar-erased: every parameter
                     * bound, some to the ENCLOSING signature's own type
                     * variable -- `(Box x)` with `x : A` inside
                     * `(defn f [A] ...)`.  That is as determined as a concrete
                     * argument: the value is `(Box A)`, and each clone knows
                     * what `A` is.  The bare-ADT fallback erased it, so in the
                     * float spec `(.val (Box x))` read the erased layout's
                     * int64 field and returned it by VALUE conversion (7.1's
                     * bits printed as 4.61968e+18).  An unbound or foreign
                     * tyvar still falls back, as before. */
                    if (!all_bound && !e->in_construct_template) {
                        bool sig_bound = true;
                        for (uint8_t pi = 0; pi < ntp && sig_bound; pi++) {
                            if (!have[pi] || targs[pi].kind == TY_UNKNOWN)
                                sig_bound = false;
                            else if (targs[pi].kind == TY_TYVAR &&
                                     !(targs[pi].as.tyvar_.name &&
                                       ng_tyvar_in_sig(e, targs[pi].as.tyvar_.name)))
                                sig_bound = false;
                        }
                        if (sig_bound) all_bound = true;
                    }
                    bool any_concrete = false;
                    for (uint8_t pi = 0; pi < ntp; pi++)
                        if (have[pi] && targs[pi].kind != TY_TYVAR &&
                            targs[pi].kind != TY_UNKNOWN)
                            any_concrete = true;
                    if (!all_bound && any_concrete) {
                        bool dyn = lang_span_is_dynamic(call->span);
                        for (uint8_t pi = 0; pi < ntp; pi++) {
                            if (have[pi] && targs[pi].kind != TY_UNKNOWN) continue;
                            if (dyn) {
                                targs[pi] = type_from_kind(TY_ANY);
                            } else {
                                targs[pi] = type_tyvar_named(ctor->adt->type_params[pi]);
                                targs[pi].as.tyvar_.open_slot = true;
                            }
                            have[pi] = true;
                        }
                        all_bound = true;
                    }
                    if (all_bound) {
                        Type adt_base = type_adt(ctor->adt);
                        adt_base.hkt_kind = kind_for_arity(ntp);
                        Type app_type = adt_base;
                        for (uint8_t pi = 0; pi < ntp; pi++)
                            app_type = type_app(e->arena, app_type, targs[pi],
                                                call->span);
                        call_expr->type = app_type;
                    }
                }
            }
            /* effect-row-lost-through-a-constructor-argument: bind a
             * control-bearing / effectful-call argument out of the constructor,
             * so the call lands at a bind position the CPS backend threads the
             * DK through instead of a ctor operand it direct-emits. */
            return elab_hoist_control_operands(e, call_expr);
        }
        /* Phase G3: Non-constructor function returning ADT — patch result from result_full_type.
         * Only recover a genuine ADT result type here; when result_full_type is a
         * bare named type variable (a polymorphic return whose body collapsed to
         * the int64 carrier, so result_kind reads TY_ADT/TY_STRUCT), elab_call_fn
         * has already instantiated the call's result from the argument types --
         * clobbering it with the uninstantiated tyvar would erase that. */
        Expr *call_expr = elab_call_fn(e, call, fn_binding);
        /* struct-return-through-closure-loses-type: only patch a genuine full
         * application (whose result really is the ADT carrier).  An
         * under-applied call returns a closure value (TY_PTR_VOID); patching it
         * to the ADT result type would mis-type the closure as the aggregate. */
        if (call_expr && call_expr->type.kind == TY_ADT &&
            fn_binding->type.as.fn.result_full_type &&
            fn_binding->type.as.fn.result_full_type->kind == TY_ADT) {
            call_expr->type = *fn_binding->type.as.fn.result_full_type;
        }
        return call_expr;
    }

    if (fn_binding && (fn_binding->type.kind == TY_FN ||
                       (fn_binding->type.kind == TY_PTR_VOID && fn_binding->closure_fn_binding) ||
                       fn_binding->closure_fn_binding)) {
        Expr *call_expr = elab_call_fn(e, call, fn_binding);
        {
            Expr *twin = typed_list_twin_redirect(e, call, fn_binding, call_expr);
            if (twin) return twin;
        }
        /* LT4: patch struct return type with full type containing StructDef pointer,
         * mirroring the G3 patch for TY_ADT above. Without this, the call expression
         * gets TY_STRUCT with def=NULL from type_from_kind(TY_STRUCT).
         * Guard on result_full_type actually being a TY_STRUCT: a polymorphic
         * return `: A` whose body is an ascription to the tyvar collapses to the
         * TY_STRUCT int64 carrier (result_kind == TY_STRUCT) while result_full_type
         * stays the named tyvar.  In that case elab_call_fn already produced the
         * instantiated result type; overwriting it with the bare tyvar would
         * discard the per-call-site substitution.  See
         * docs/archive/history/parameterized-defopaque.md. */
        /* struct-return-through-closure-loses-type: gate on the call result
         * actually being the struct carrier.  An under-applied call returns a
         * closure value (TY_PTR_VOID); patching it to the struct result type
         * would mis-type the closure as the by-value struct (so a later
         * application is mis-routed -- e.g. CTOR-V0 sees the binding as a struct
         * name).  A full application's result kind is already TY_STRUCT here. */
        if (call_expr && call_expr->type.kind == TY_STRUCT &&
            fn_binding->type.kind == TY_FN &&
            fn_binding->type.as.fn.result_kind == TY_STRUCT &&
            fn_binding->type.as.fn.result_full_type &&
            fn_binding->type.as.fn.result_full_type->kind == TY_STRUCT) {
            call_expr->type = *fn_binding->type.as.fn.result_full_type;
        }
        /* F1-1: patch TY_EXISTS / TY_FORALL return type with the full
         * forall_ payload so `open` (and any other downstream consumer
         * that dereferences `as.forall_.body`) sees a populated struct
         * instead of a zero-initialised type_from_kind() shell. */
        if (call_expr &&
            (call_expr->type.kind == TY_EXISTS || call_expr->type.kind == TY_FORALL) &&
            fn_binding->type.kind == TY_FN &&
            (fn_binding->type.as.fn.result_kind == TY_EXISTS ||
             fn_binding->type.as.fn.result_kind == TY_FORALL) &&
            fn_binding->type.as.fn.result_full_type) {
            call_expr->type = *fn_binding->type.as.fn.result_full_type;
        }
        return call_expr;
    }

    /* Phase 19: Allow calling any binding (for function parameters, higher-order functions) */
    if (fn_binding) {
        return elab_call_fn(e, call, fn_binding);
    }

    /* Builtin operator. Evaluate args first, then look up. */
    uint32_t n_args = call->as.list.len - 1;

    /* N0 (numeric-tower-rational-complex-plan §3): the builtin operator rows
     * are keyed by TypeKind and emit a C infix operator, so they can never
     * express `Rational + Rational`.  Peek at the first argument's type BEFORE
     * committing to the builtin path: when no builtin row matches but a `Num`
     * instance does, the call is `Num` typeclass dispatch, not an error.
     *
     * The peek happens before the move-tracking loop below on purpose -- the
     * Num path re-elaborates the argument forms inside elab_method_call, and a
     * move already poisoned here would surface as a bogus use-after-move.  On
     * the builtin path the peeked expression is reused verbatim, so an ordinary
     * `(+ a b)` still elaborates each argument exactly once. */
    Expr *arg0_peek = NULL;
    if (n_args > 0) {
        arg0_peek = elab_form(e, call->as.list.items[1]);
        if (!arg0_peek) return NULL;
    }
    Type first_t = (n_args > 0) ? arg0_peek->type : TYPE_NIL;
    const BuiltinSpec *spec = builtin_lookup(name, first_t, n_args);
    if (!spec) {
        Expr *num_disp = elab_try_num_operator_dispatch(e, call, head, name,
                                                        &first_t, n_args);
        if (num_disp) return num_disp;
    }

    Expr **args = (n_args == 0) ? NULL :
        (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = (i == 0) ? arg0_peek : elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) return NULL;
        /* Phase 11: Move tracking - if arg is a CK_MOVE binding reference, poison it.
         * UT2 exception: ^unique ^mut bindings represent exclusive mutable access;
         * builtins never take unique ownership, so don't consume them. */
        if (args[i]->kind == EX_VAR && type_is_move(args[i]->as.var.binding->type)) {
            Binding *arg_b2 = args[i]->as.var.binding;
            bool arg_is_unique_mut = arg_b2->is_unique && arg_b2->is_mut;
            if (!arg_is_unique_mut) {
                binding_mark_moved(arg_b2, args[i]->span);
            }
        }
    }
    /* saffron-lang-plan S3/D4: in a Saffron file, a builtin applied to an `any`
     * argument is resolved at RUNTIME rather than rejected.
     *
     * The builtin table is keyed by the first argument's TypeKind, so `(* x 2)`
     * with `x : any` finds no row -- which in Turmeric is the right answer and
     * in Saffron is the ordinary case.  Route it to EX_DYN_OP, whose operator
     * is chosen from the value's own tag when it runs.
     *
     * Three conditions, each narrowing:
     *   - the file is Saffron (per-span, so a loaded Turmeric module keeps the
     *     static error);
     *   - the NAME is a real builtin, so a typo is still TUR-E0006 rather than
     *     a runtime failure -- `builtin_first_with_name` is the same check the
     *     diagnostic below already makes to decide it has something to say;
     *   - at least one argument is actually `any`.  A call whose arguments are
     *     all concrete and still finds no row is a genuine type error, and
     *     making it dynamic would hide it.
     *
     * Note this does NOT require the static lookup to have failed.  `(* 2 x)`
     * finds a row -- the table is keyed on argument ZERO, and that one is an
     * int -- and then rejects `x` as "arg 2: expected int, got any".  An
     * operator is dynamic when ANY operand is, not only when the first is, so
     * the whole call goes dynamic and the position of the `any` stops
     * mattering. */
    if (lang_span_is_dynamic(call->span) && builtin_first_with_name(name)) {
        bool has_any = false;
        for (uint32_t i = 0; i < n_args; i++)
            if (args[i] && args[i]->type.kind == TY_ANY) { has_any = true; break; }
        if (has_any) {
            Type any_t;
            memset(&any_t, 0, sizeof(any_t));
    any_t.copy_kind = CK_COPY;   /* CK_UNIQUE is 0: a zeroed type is unique-kinded (saffron-any-let-binding-is-unique) */
            any_t.kind = TY_ANY;
            /* saffron-lang-plan S5: widen every operand, not only the `any`
             * ones.  A mixed call like `(* x 2)` is the common shape, and the
             * two halves disagree about what to do with the concrete operand:
             * the interpreter does not care, because a TuriValue is uniform
             * whatever the static type said, while the compiled runtime takes
             * `tur_tagged_t` and would be handed a bare `int64_t` for the `2`.
             *
             * Widening HERE rather than in the emitter keeps the node's own
             * invariant honest -- every operand of a dynamic operator is `any`
             * -- so both back ends read the same tree and neither has to
             * reconstruct the coercion.  It costs the interpreter nothing: an
             * EX_UNION_INJECT over an int is a transparent shim there. */
            for (uint32_t i = 0; i < n_args; i++) {
                Expr *w = elab_coerce_to_any(e, args[i]);
                if (w) args[i] = w;
            }
            Expr *dyn = expr_new(e->arena, EX_DYN_OP, any_t, call->span);
            dyn->as.dyn_op_.op     = name;
            dyn->as.dyn_op_.args   = args;
            dyn->as.dyn_op_.n_args = n_args;
            return elab_hoist_control_operands(e, dyn);
        }
    }

    if (!spec) {
        const BuiltinSpec *any = builtin_first_with_name(name);
        if (any) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0006_OPERATOR_LOOKUP_FAILED,
                                "operator lookup failed for '%s': got %u arg(s), first arg type %s",
                                name->name, n_args,
                                n_args > 0 ? type_name(first_t) : "<none>");

            const BuiltinSpec *overloads[32];
            uint32_t n_overloads = builtin_collect_with_name(name, overloads, 32);
            for (uint32_t oi = 0; oi < n_overloads; oi++) {
                const BuiltinSpec *ov = overloads[oi];
                const char *arg_name = (ov->arg_type.kind == TY_UNKNOWN)
                    ? "any"
                    : type_name(ov->arg_type);
                const char *res_name = type_name(ov->result_type);
                if (ov->max_arity < 0) {
                    diag_emit(DIAG_NOTE, call->span,
                              "available overload: %s arity %d..* arg=%s result=%s",
                              ov->name, ov->min_arity, arg_name, res_name);
                } else {
                    diag_emit(DIAG_NOTE, call->span,
                              "available overload: %s arity %d..%d arg=%s result=%s",
                              ov->name, ov->min_arity, ov->max_arity, arg_name, res_name);
                }
            }
        } else if (e->separate_compilation || !g_interpret_mode ||
                   /* r7rs-turmeric-syntax-leaks item 9: a Scheme program
                    * names only what it defines or imports, so an unknown
                    * name is an error on both back ends -- never the
                    * interpreter's runtime dispatch to a native. */
                   scheme_span_is_user_source(head->span)) {
            /* UCH1 (diagnose-unbound-call-heads-plan): in any compiled path an
             * unknown call head is a genuine unbound reference (a typo, or a
             * missing import / extern-c).  Report it here instead of silently
             * typing the call :int -- which previously let `tur check` pass and
             * deferred the failure to a cryptic C-compiler "undeclared" error,
             * or surfaced as a misleading downstream type mismatch.  The
             * runtime-dispatch fallback below is reserved for interpret mode
             * (eval / --interpret / repl / worker), where TuriEnv natives are
             * resolved at runtime. */
            const char *hint_file = stdlib_load_hint_file(name);
            if (hint_file) {
                char err_msg[128];
                char hint_text[160];
                char hint_repl[160];
                snprintf(err_msg, sizeof(err_msg),
                         "unknown function or operator '%s'", name->name);
                snprintf(hint_text, sizeof(hint_text),
                         "'%s' lives in %s and is not auto-loaded",
                         name->name, hint_file);
                snprintf(hint_repl, sizeof(hint_repl),
                         "(load \"%s\")", hint_file);
                DiagSuggestion sug = { hint_text, hint_repl, NULL };
                diag_emit_with_suggestion(DIAG_ERROR, head->span, err_msg, &sug);
            } else if (tur_legacy_form_hint(name->name) ||
                       struct_accessor_hint(e, name->name, NULL, 0)) {
                /* Legacy C-backed-spice forms (sizeof / float64* / declare) and
                 * `<struct>-<field>` accessor-function calls: emit a migration
                 * pointer instead of a bare "unknown" (UCH1 + spice v0.21
                 * compat). */
                char err_msg[128];
                char acc_buf[256];
                const char *hint = tur_legacy_form_hint(name->name);
                if (!hint && struct_accessor_hint(e, name->name, acc_buf,
                                                  sizeof(acc_buf)))
                    hint = acc_buf;
                snprintf(err_msg, sizeof(err_msg),
                         "unknown function or operator '%s'", name->name);
                DiagSuggestion sug = { hint, NULL, NULL };
                diag_emit_with_suggestion(DIAG_ERROR, head->span, err_msg, &sug);
            } else {
                diag_emit(DIAG_ERROR, head->span,
                          "unknown function or operator '%s'", name->name);
            }
        } else {
            /* eval mode: create a runtime-dispatch call so native builtins
             * registered in TuriEnv (e.g. async scheduler functions) are
             * callable without a compile-time declaration.
             * The binding is NOT added to any scope so future lookups don't
             * find a TYPE_INT entry and route through elab_call_fn.
             *
             * Known interpreter natives whose return type is not :int get
             * explicit typing here so callers (e.g. `(if (error? r) ...)`)
             * see the right type at the call site.  Add new entries
             * sparingly -- a stdlib declaration is preferred when one fits.
             *
             * Embedder natives registered with a typed registration API
             * (turi_register_default_native_typed / turi_env_register_native_typed)
             * record their return type in the process-global signature registry;
             * consult it so a curated typed wrapper over a non-:int native
             * elaborates correctly.  See
             * docs/archive/history/untyped-native-registration-blocks-curated-facades.md. */
            Type dispatch_result = TYPE_INT;
            const char *nm = name->name;
            bool native_registered = false;
            if (nm) {
                if      (strcmp(nm, "error?") == 0)        { dispatch_result = TYPE_BOOL; native_registered = true; }
                else if (strcmp(nm, "error-message") == 0) { dispatch_result = TYPE_CSTR; native_registered = true; }
                else {
                    TurNativeRetType nrt;
                    if (tur_native_sig_lookup(nm, &nrt)) {
                        native_registered = true;
                        switch (nrt) {
                            case TUR_NRT_FLOAT:  dispatch_result = TYPE_FLOAT;    break;
                            case TUR_NRT_BOOL:   dispatch_result = TYPE_BOOL;     break;
                            case TUR_NRT_CSTR:   dispatch_result = TYPE_CSTR;     break;
                            case TUR_NRT_VOID:   dispatch_result = TYPE_NIL;      break;
                            case TUR_NRT_PTR:    dispatch_result = TYPE_PTR_VOID; break;
                            case TUR_NRT_SYNTAX: dispatch_result = TYPE_SYNTAX;   break;
                            case TUR_NRT_INT:   /* fallthrough: keep TYPE_INT */
                            default:            dispatch_result = TYPE_INT;      break;
                        }
                    }
                }
            }
            /* The runtime-dispatch fallback is correct for natives registered
             * after elaboration (async scheduler functions, embedder natives via
             * turi_env_register_native).  But it also silently swallows typos: an
             * undefined name typed :int here passes `_validate` and only fails at
             * runtime when the call actually runs (and only on the branches that
             * run).  When the name is neither bound nor in the typed-native
             * registry, warn so embedders consuming the diag sink surface it at
             * load time.  See
             * docs/archive/history/eval-mode-unknown-call-deferred-to-runtime.md. */
            if (!native_registered) {
                diag_emit_with_code(DIAG_WARNING, head->span,
                                    TUR_W0040_EVAL_UNKNOWN_CALL_RUNTIME_DISPATCH,
                                    "unknown name '%s'; will runtime-dispatch -- typo?",
                                    nm ? nm : "<null>");
            }
            Binding *dyn_b = binding_new(e, name, dispatch_result, false, false, head->span);
            Expr *var_expr = expr_new(e->arena, EX_VAR, dispatch_result, head->span);
            var_expr->as.var.binding = dyn_b;
            Expr *out = expr_new(e->arena, EX_CALL, dispatch_result, call->span);
            out->as.call_.fn_binding = NULL;
            out->as.call_.fn_expr    = var_expr;
            out->as.call_.args       = args;
            out->as.call_.n_args     = n_args;
            return out;
        }
        return NULL;
    }
    /* The `cons` builtin (BS_FUNC_CALL, c_op="cons") accepts any 64-bit-sized
     * value for head and tail -- ints, cstrs, opaque handles, pointers --
     * since cells are pointer-as-int64.  Bypass the strict per-arg check for
     * it; codegen casts the args through intptr_t.  See
     * docs/archive/history/cons-builtin-rejects-cstr-head.md. */
    bool cons_wildcard = (spec->shape == BS_FUNC_CALL && spec->c_op &&
                          strcmp(spec->c_op, "cons") == 0);
    /* All args must match the spec's arg type. */
    for (uint32_t i = 0; i < n_args; i++) {
        if (cons_wildcard) continue;
        if (!type_eq(args[i]->type, spec->arg_type)) {
            const char *expected_str = type_name(spec->arg_type);
            const char *actual_str = type_name(args[i]->type);

            if (typekind_is_numeric(args[i]->type.kind) &&
                       typekind_is_numeric(spec->arg_type.kind)) {
                /* Phase B: TUR-E0042 -- no implicit coercion between distinct numeric kinds */
                diag_emit_with_code(DIAG_ERROR, args[i]->span, TUR_E0042_MIXED_WIDTH_ARITH,
                                    "mixed-width numeric arithmetic: '%s' arg %u is %s, expected %s",
                                    name->name, i + 1, actual_str, expected_str);
                diag_emit(DIAG_HELP, args[i]->span,
                          "use (as %s expr) for explicit numeric conversion", expected_str);
            } else {
                diag_emit_with_code(DIAG_ERROR, args[i]->span, TUR_E0001_TYPE_MISMATCH,
                                    "'%s' arg %u: type mismatch - expected %s, got %s",
                                    name->name, i + 1, expected_str, actual_str);
                if (args[i]->type.kind == TY_BOOL && spec->arg_type.kind == TY_INT) {
                    diag_emit(DIAG_HELP, args[i]->span, "try wrapping the bool in (if x 1 0)");
                }
            }
            diag_emit(DIAG_NOTE, args[i]->span, "argument has this type");
            return NULL;
        }
    }
    /* The static builtin spec table initializes result_type with a designated
     * initializer (`{.kind=TY_FLOAT}` etc.) that leaves `.copy_kind` zeroed --
     * which is CK_UNIQUE/CK_MOVE, not the kind's true copy semantics. Left
     * uncorrected, a `let`-bound arithmetic result (e.g. `(- 0.0 a)`) inherits
     * copy_kind=CK_MOVE, so reading it twice through any builtin trips a bogus
     * TUR-E0005 use-after-move on a Copy primitive. Stamp the canonical
     * copy_kind for the result's kind (a no-op for genuinely move-only results
     * like rc<T>/weak<T>, whose default is already CK_MOVE). */
    Type result_type = spec->result_type;
    result_type.copy_kind = typekind_default_copy_kind(result_type.kind);
    /* prelude-macros (Defect B / F3): record that the `cons` runtime list
     * constructor is referenced so the emitter injects its cons-cell helper
     * into this TU's preamble.  Keyed on c_op identity (the only BS_FUNC_CALL
     * builtin whose C name is "cons"). */
    if (spec->shape == BS_FUNC_CALL && spec->c_op &&
        strcmp(spec->c_op, "cons") == 0) {
        extern bool g_uses_cons;
        g_uses_cons = true;
    }
    Expr *out = expr_new(e->arena, EX_BUILTIN, result_type, call->span);
    out->as.builtin.spec = spec;
    out->as.builtin.args = args;
    out->as.builtin.n = n_args;
    return out;
}

/* CY1: Partially apply a function, returning a closure over the provided args.
 *
 * fn_type   -- the EFFECTIVE function type (thunk type if closure, including env param)
 * fn_binding -- the binding being called (closure_fn_binding != NULL iff it's a closure)
 * elab_args -- already-elaborated argument expressions [0..n_provided-1]
 * n_provided -- number of arguments already provided (< full_arity)
 */
static Expr *elab_partial_apply(Elab *e, const Form *call, Binding *fn_binding,
                                 Type fn_type, Expr **elab_args, uint32_t n_provided) {
    bool fn_is_closure = (fn_binding->closure_fn_binding != NULL);

    /* full_arity = user-visible arg count (strips env from thunk arity) */
    uint32_t full_arity = fn_type.as.fn.arity;
    if (fn_is_closure) full_arity--; /* strip hidden env param */

    uint32_t n_remaining = full_arity - n_provided;
    TypeKind result_kind = fn_type.as.fn.result_kind;

    /* CY4: helper -- return the full type for original-arg user-slot `idx`
     * (0-based, env-stripped). NULL if the slot is monomorphic. */
    Type *const *src_full_types = fn_type.as.fn.arg_full_types;
    #define PAP_SLOT_FULL(idx) \
        (src_full_types ? src_full_types[fn_is_closure ? ((idx) + 1) : (idx)] : NULL)

    /* Build capture bindings for provided args */
    Binding **cap_bindings = (Binding **)arena_alloc(e->arena, n_provided * sizeof(Binding *));
    for (uint32_t i = 0; i < n_provided; i++) {
        char cap_name[32];
        snprintf(cap_name, sizeof(cap_name), "__papc%u", elab_fresh_id(e));
        const Symbol *cap_sym = symtab_intern(e->st, strslice(cap_name, (uint32_t)strlen(cap_name)));
        /* arg type from fn_type: skip index 0 if closure (env), so index = i+1 if closure, else i */
        TypeKind cap_kind = fn_type.as.fn.arg_kinds[fn_is_closure ? (i + 1) : i];
        /* Type-check captured (partial-application) args against the slot they
         * fill.  Mirror the saturated positional check, which for a
         * struct/opaque/ADT parameter is strict: the captured argument must be
         * the *same* nominal type -- not merely the same TypeKind, and not a
         * value of a differing kind (e.g. a bare int) at all.  Two failure
         * modes are folded together here:
         *   - kind-level: a plain int (TY_INT) captured at a TY_STRUCT opaque
         *     slot -- differing kinds.  Without this, `(two 5)` -- binding an
         *     int into a :A slot -- slips through because the capture loop never
         *     compared the provided arg's type to the parameter at all.
         *   - nominal-identity: a :B captured at a :A slot -- same kind,
         *     different nominal.  The saturated path only re-checks the
         *     *remaining* params, so the captured slot must be validated here.
         * See docs/archive/history/partial-application-skips-captured-arg-type-check.md
         * and docs/archive/history/positional-nominal-type-identity-fix-plan.md.
         *
         * A session / role endpoint slot is checked the same way: `type_eq`
         * compares its protocol (equirecursively) and role names, exactly as
         * the saturated positional check does. A protocol is structural, not
         * nominal, hence the gate's name. Without it, under-saturating a call
         * was a way around the protocol check a saturated call performs
         * (docs/archive/pap-captured-arg-skips-session-role-protocol-check.md). */
        {
            Type *cap_full_chk = PAP_SLOT_FULL(i);
            #define PAP_FULL_CHECK_KIND(k) \
                ((k) == TY_STRUCT || (k) == TY_ADT || (k) == TY_SESSION || (k) == TY_ROLE)
            bool slot_needs_full_type_check =
                (cap_full_chk && PAP_FULL_CHECK_KIND(cap_full_chk->kind)) ||
                PAP_FULL_CHECK_KIND(cap_kind);
            if (slot_needs_full_type_check) {
                /* Prefer the recorded full type for an exact compare; fall
                 * back to a kind-level compare when it is unavailable. */
                bool mismatch;
                Type expected_ty;
                if (cap_full_chk && PAP_FULL_CHECK_KIND(cap_full_chk->kind)) {
                    mismatch = !type_eq(elab_args[i]->type, *cap_full_chk);
                    expected_ty = *cap_full_chk;
                } else {
                    mismatch = (elab_args[i]->type.kind != cap_kind);
                    expected_ty = type_from_kind(cap_kind);
                }
                if (mismatch) {
                    Buf eb; buf_init(&eb);
                    type_print(&eb, expected_ty); buf_putc(&eb, '\0');
                    Buf ab; buf_init(&ab);
                    type_print(&ab, elab_args[i]->type); buf_putc(&ab, '\0');
                    diag_emit_with_code(DIAG_ERROR, elab_args[i]->span,
                                        TUR_E0001_TYPE_MISMATCH,
                                        "function '%s' arg %u: expected %s, got %s",
                                        fn_binding->name->name, i + 1, eb.data, ab.data);
                    buf_free(&eb); buf_free(&ab);
                    return NULL;
                }
            }
            #undef PAP_FULL_CHECK_KIND
        }
        Type cap_type = type_from_kind(cap_kind);
        /* A5: a captured struct/ADT slot must carry its *full* nominal type, not
         * the kind-erased TY_STRUCT/TY_ADT.  Otherwise the env field is emitted
         * as int64_t (type_c_name of a nameless struct kind), the let-binding
         * init truncates the struct value, and the inner call passes an int64_t
         * where the callee expects the nominal struct -- a hard C compile error.
         * See docs/archive/history/stdlib-type-erasure-cleanup-plan.md (A5). */
        {
            Type *cap_full_t = PAP_SLOT_FULL(i);
            if (cap_full_t &&
                    (cap_full_t->kind == TY_STRUCT || cap_full_t->kind == TY_ADT)) {
                cap_type = *cap_full_t;
            }
        }
        Binding *cap_b = binding_new(e, cap_sym, cap_type, false, false, call->span);
        /* CY4: rank-2 captured arg -- carry forall info onto the binding so
         * the closure env field is emitted as tur_poly_fn_t. */
        Type *cap_full = PAP_SLOT_FULL(i);
        if (cap_full && cap_full->kind == TY_FORALL) {
            cap_b->is_poly_fn = true;
            cap_b->poly_type = cap_full;
        }
        cap_bindings[i] = cap_b;
    }

    /* Build remaining param bindings for the new thunk */
    Binding **rem_params = (Binding **)arena_alloc(e->arena, n_remaining * sizeof(Binding *));
    TypeKind rem_kinds[MAX_FN_ARITY];
    for (uint32_t i = 0; i < n_remaining; i++) {
        char rem_name[32];
        snprintf(rem_name, sizeof(rem_name), "__papr%u", elab_fresh_id(e));
        const Symbol *rem_sym = symtab_intern(e->st, strslice(rem_name, (uint32_t)strlen(rem_name)));
        TypeKind rem_kind = fn_type.as.fn.arg_kinds[fn_is_closure ? (n_provided + 1 + i) : (n_provided + i)];
        Type rem_type = type_from_kind(rem_kind);
        Binding *rem_b = binding_new(e, rem_sym, rem_type, false, false, call->span);
        /* CY4: rank-2 remaining param -- mark so the thunk's C signature uses
         * tur_poly_fn_t for that slot. */
        Type *rem_full = PAP_SLOT_FULL(n_provided + i);
        if (rem_full && rem_full->kind == TY_FORALL) {
            rem_b->is_poly_fn = true;
            rem_b->poly_type = rem_full;
        }
        rem_params[i] = rem_b;
        rem_kinds[i] = rem_kind;
    }

    /* Build the inner call expression: (fn_binding cap0 cap1 ... rem0 rem1 ...) */
    /* n_call_args = full_arity (all user-visible args) */
    uint32_t n_call_args = full_arity;
    Expr **call_args = (Expr **)arena_alloc(e->arena, n_call_args * sizeof(Expr *));
    for (uint32_t i = 0; i < n_provided; i++) {
        Expr *var = expr_new(e->arena, EX_VAR, cap_bindings[i]->type, call->span);
        var->as.var.binding = cap_bindings[i];
        /* CY4: rank-2 capture is already stored as tur_poly_fn_t; pass through. */
        if (cap_bindings[i]->is_poly_fn) {
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, call->span);
            wrap->as.poly_wrap_.inner = var;
            wrap->as.poly_wrap_.wrapper_binding = NULL;
            wrap->as.poly_wrap_.is_closure = false;
            call_args[i] = wrap;
        } else {
            call_args[i] = var;
        }
    }
    for (uint32_t i = 0; i < n_remaining; i++) {
        Expr *var = expr_new(e->arena, EX_VAR, rem_params[i]->type, call->span);
        var->as.var.binding = rem_params[i];
        /* CY4: rank-2 remaining param arrives wrapped as tur_poly_fn_t; pass through. */
        if (rem_params[i]->is_poly_fn) {
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, call->span);
            wrap->as.poly_wrap_.inner = var;
            wrap->as.poly_wrap_.wrapper_binding = NULL;
            wrap->as.poly_wrap_.is_closure = false;
            call_args[n_provided + i] = wrap;
        } else {
            call_args[n_provided + i] = var;
        }
    }

    /* struct-return-through-closure-loses-type (CURRY-V1): when the underlying
     * function returns a by-value aggregate (struct/ADT), the inner call -- and
     * therefore the thunk's body -- must carry the FULL result type, with its
     * StructDef/AdtDef, not the def-less type_from_kind(result_kind).  Otherwise
     * emit lowers the thunk's C return type to the int64 carrier (a nameless
     * struct kind lowers to int64_t) while the body returns the real struct by
     * value -- a hard C compile error ("returning Person but int64_t expected").
     * The thunk is invoked through its typed `tur_thunk_<R>_..._t` slot, so a
     * by-value struct return round-trips correctly with no boxing. */
    /* struct-curry-ctor: a partial application of a record-ADT CONSTRUCTOR.  A
     * ctor binding carries result_kind TY_ADT but NO result_full_type (the
     * direct-call path patches the AdtDef in after elab_call_fn).  Without it
     * the CURRY-V1 logic below would type the thunk body / completion as a
     * def-less TY_ADT, so the completed value's `.field` access cannot resolve
     * the AdtDef.  Recover the ctor's def and stamp a synthetic full ADT result
     * type so the thunk and its completion carry it.  (The thunk body's inner
     * ctor call still emits `ctor_Name` because the ORIGINAL ctor binding has no
     * result_full_type -- the emit ctor-branch gate stays satisfied.) */
    CtorDef *pap_ctor = (!fn_is_closure && result_kind == TY_ADT)
        ? elab_lookup_ctor(e, fn_binding->name) : NULL;
    Type *ctor_result_full = NULL;
    if (pap_ctor && pap_ctor->adt) {
        ctor_result_full = (Type *)arena_alloc(e->arena, sizeof(Type));
        *ctor_result_full = type_adt(pap_ctor->adt);
    }
    Type body_result_type =
        ctor_result_full ? *ctor_result_full :
        ((fn_type.as.fn.result_full_type &&
          (fn_type.as.fn.result_full_type->kind == TY_STRUCT ||
           fn_type.as.fn.result_full_type->kind == TY_ADT))
            ? *fn_type.as.fn.result_full_type
            : type_from_kind(result_kind));
    Expr *inner_call = expr_new(e->arena, EX_CALL, body_result_type, call->span);
    inner_call->as.call_.fn_binding = fn_binding;
    inner_call->as.call_.args = call_args;
    inner_call->as.call_.n_args = n_call_args;
    inner_call->as.call_.fn_expr = NULL;
    inner_call->as.call_.dict_arg = NULL;
    inner_call->as.call_.is_poly_call = false;
    inner_call->as.call_.poly_arg_mask = 0;

    /* Build the thunk FnDef */
    /* Thunk params: [env_param (TY_PTR_VOID), rem_param_0, ..., rem_param_{n_remaining-1}] */
    uint8_t thunk_n_params = (uint8_t)(1 + n_remaining);
    Binding **thunk_params = (Binding **)arena_alloc(e->arena, thunk_n_params * sizeof(Binding *));
    Type *thunk_param_types = (Type *)arena_alloc(e->arena, thunk_n_params * sizeof(Type));

    /* env param */
    char env_param_name[32];
    snprintf(env_param_name, sizeof(env_param_name), "__pap_env_%u", elab_fresh_id(e));
    const Symbol *env_param_sym = symtab_intern(e->st, strslice(env_param_name, (uint32_t)strlen(env_param_name)));
    Binding *env_param_b = binding_new(e, env_param_sym, TYPE_PTR_VOID, false, false, call->span);
    thunk_params[0] = env_param_b;
    thunk_param_types[0] = TYPE_PTR_VOID;

    for (uint32_t i = 0; i < n_remaining; i++) {
        thunk_params[1 + i] = rem_params[i];
        thunk_param_types[1 + i] = type_from_kind(rem_kinds[i]);
    }

    /* Thunk type: (TY_PTR_VOID, rem_kinds...) -> result_kind */
    TypeKind thunk_arg_kinds[MAX_FN_ARITY];
    thunk_arg_kinds[0] = TY_PTR_VOID;
    for (uint32_t i = 0; i < n_remaining; i++) {
        thunk_arg_kinds[1 + i] = rem_kinds[i];
    }
    Type thunk_type = type_fn(thunk_arg_kinds, thunk_n_params, result_kind);

    /* CY4: Propagate effect_row from the original function. A partial
     * application of an effectful function must retain the same effect row so
     * the effect-check pass and any later call sites see the effects when the
     * resulting closure is invoked. */
    thunk_type.as.fn.effect_row = fn_type.as.fn.effect_row;

    /* CY4: Propagate arg_full_types and result_full_type for the remaining
     * parameters. This preserves rank-2 polymorphic parameter types and any
     * non-scalar full type info so the resulting closure can still accept
     * forall-typed arguments. */
    if (fn_type.as.fn.arg_full_types) {
        Type **rem_full = (Type **)arena_alloc(e->arena, thunk_n_params * sizeof(Type *));
        rem_full[0] = NULL; /* env */
        for (uint32_t i = 0; i < n_remaining; i++) {
            uint32_t src_idx = fn_is_closure ? (n_provided + 1 + i) : (n_provided + i);
            rem_full[1 + i] = fn_type.as.fn.arg_full_types[src_idx];
        }
        thunk_type.as.fn.arg_full_types = rem_full;
    }
    if (fn_type.as.fn.result_full_type) {
        thunk_type.as.fn.result_full_type = fn_type.as.fn.result_full_type;
    } else if (ctor_result_full) {
        /* struct-curry-ctor: carry the recovered ADT result on the thunk so the
         * completed closure's value type resolves `.field` access. */
        thunk_type.as.fn.result_full_type = ctor_result_full;
    }

    /* Thunk binding (global) */
    char pap_name[32];
    snprintf(pap_name, sizeof(pap_name), "__pap%u", elab_fresh_id(e));
    const Symbol *pap_sym = symtab_intern(e->st, strslice(pap_name, (uint32_t)strlen(pap_name)));
    Binding *thunk_binding = binding_new(e, pap_sym, thunk_type, false, true, call->span);
    scope_add(&e->global, thunk_binding);

    /* Build FnDef */
    FnDef *pap_fd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    memset(pap_fd, 0, sizeof(FnDef));
    pap_fd->binding = thunk_binding;
    pap_fd->params = thunk_params;
    pap_fd->n_params = thunk_n_params;
    pap_fd->param_types = thunk_param_types;
    pap_fd->body = inner_call;
    pap_fd->is_variadic = false;
    pap_fd->inferred_effect_row = NULL;
    constraint_set_init(&pap_fd->constraints);

    /* Build the env struct name */
    char pap_env_name[32];
    snprintf(pap_env_name, sizeof(pap_env_name), "__pap_env_s_%u", elab_fresh_id(e));
    const Symbol *pap_env_sym = symtab_intern(e->st, strslice(pap_env_name, (uint32_t)strlen(pap_env_name)));

    /* Build captures list: [cap_binding[0], ..., cap_binding[n_provided-1]] + fn_binding if closure */
    uint32_t n_pap_captures = n_provided + (fn_is_closure ? 1 : 0);
    Binding **pap_captures = (Binding **)arena_alloc(e->arena, (n_pap_captures ? n_pap_captures : 1) * sizeof(Binding *));
    for (uint32_t i = 0; i < n_provided; i++) {
        pap_captures[i] = cap_bindings[i];
    }
    if (fn_is_closure) {
        pap_captures[n_provided] = fn_binding;
    }

    /* Build Closure struct */
    struct Closure *pap_closure = (struct Closure *)arena_alloc(e->arena, sizeof(struct Closure));
    pap_closure->fn = pap_fd;
    pap_closure->captures = pap_captures;
    pap_closure->n_captures = (uint8_t)n_pap_captures;
    pap_closure->env_name = pap_env_sym;
    pap_closure->is_shift_receiver = false;   /* arena mem is not zeroed */
    pap_closure->fat_captures_borrowed = false;
    pap_closure->is_effect_payload = false;
    pap_closure->capture_drop_insts = NULL;   /* Model R #1b: no Drop resolution here */
    pap_closure->capture_clone_insts = NULL;

    /* Wire closure into FnDef (required for emit_fn_def to emit the env struct) */
    pap_fd->closure = pap_closure;

    /* Register thunk at file scope */
    Expr *fn_def_expr = expr_new(e->arena, EX_FN_DEF, thunk_type, call->span);
    fn_def_expr->as.fn_def_.fn = pap_fd;
    elab_register_file_def(e, fn_def_expr);

    /* partial-application-widened-to-any-is-a-ptr (PROBE): the curried value's
     * user-visible type is a function of the REMAINING parameters, not an
     * untyped pointer. */
    Type pap_value_type = type_fn(rem_kinds, n_remaining, result_kind);
    pap_value_type.as.fn.boxed = true;
    pap_value_type.as.fn.effect_row = fn_type.as.fn.effect_row;
    pap_value_type.as.fn.result_full_type = thunk_type.as.fn.result_full_type;
    if (thunk_type.as.fn.arg_full_types) {
        Type **pv_full = (Type **)arena_alloc(e->arena,
                             (n_remaining ? n_remaining : 1) * sizeof(Type *));
        for (uint32_t i = 0; i < n_remaining; i++)
            pv_full[i] = thunk_type.as.fn.arg_full_types[1 + i];
        pap_value_type.as.fn.arg_full_types = pv_full;
    }

    /* Build EX_CLOSURE */
    Expr *closure_expr = expr_new(e->arena, EX_CLOSURE, pap_value_type, call->span);
    closure_expr->as.closure_.closure = pap_closure;

    if (n_provided == 0) {
        /* Edge case: no args provided, just return the closure directly */
        return closure_expr;
    }

    /* Wrap in EX_LET: let [cap0 = arg0, cap1 = arg1, ...] closure_expr */
    LetBinding *let_bs = (LetBinding *)arena_alloc(e->arena, n_provided * sizeof(LetBinding));
    for (uint32_t i = 0; i < n_provided; i++) {
        let_bs[i].binding = cap_bindings[i];
        let_bs[i].init = elab_args[i];
        /* CY4: When the captured slot is rank-2, wrap the user's argument so
         * the value stored in the env is a tur_poly_fn_t. Pass-through if the
         * argument is itself already a poly fn binding. */
        if (cap_bindings[i]->is_poly_fn) {
            Binding *inner_fn_b = poly_arg_fn_binding(elab_args[i]);
            if (!inner_fn_b) {
                diag_emit(DIAG_ERROR, elab_args[i]->span,
                          "rank-2 argument must be a named function (capturing closures not yet supported)");
                return NULL;
            }
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, elab_args[i]->span);
            wrap->as.poly_wrap_.inner = elab_args[i];
            wrap->as.poly_wrap_.is_closure = false;
            if (inner_fn_b->is_poly_fn) {
                wrap->as.poly_wrap_.wrapper_binding = NULL;
            } else {
                uint32_t inner_arity = (inner_fn_b->type.kind == TY_FN)
                    ? inner_fn_b->type.as.fn.arity : 1;
                if (inner_fn_b->closure_fn_binding) inner_arity--;
                Binding *wrapper_b = make_poly_wrapper(e, inner_fn_b, inner_arity, elab_args[i]->span, false);
                if (!wrapper_b) return NULL;
                wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
            }
            let_bs[i].init = wrap;
        }
    }
    #undef PAP_SLOT_FULL

    Expr *let_expr = expr_new(e->arena, EX_LET, pap_value_type, call->span);
    let_expr->as.let_.bindings = let_bs;
    let_expr->as.let_.n = n_provided;
    let_expr->as.let_.body = closure_expr;

    return let_expr;
}

/* bare-fat-result-monomorphization (Phase B) -------------------------------
 * See docs/archive/history/bare-fat-result-monomorphization-plan.md. */

/* Recover the result kind of a closure passed into a bare-^fat slot, from any
 * of its surface forms: a bare/auto-shimmed function value (TY_FN, possibly
 * wrapped in EX_FN_TO_FAT / EX_ASCRIBE), a capturing-closure value (TY_PTR_VOID
 * with a closure_fn_binding / returns_closure_fn_binding), or a plain fn-typed
 * variable.  TY_UNKNOWN when it cannot be recovered (e.g. a bare-^fat param
 * passed through, which carries no signature -- the recursive case). */
static TypeKind bare_fat_arg_result_kind(const Expr *a) {
    while (a) {
        if (a->kind == EX_FN_TO_FAT) { a = a->as.fn_to_fat_.inner; continue; }
        if (a->kind == EX_ASCRIBE)   { a = a->as.ascribe_.inner;   continue; }
        break;
    }
    if (!a) return TY_UNKNOWN;
    if (a->type.kind == TY_FN) return a->type.as.fn.result_kind;
    Binding *cl = expr_closure_fn_binding(a);
    if (cl && cl->type.kind == TY_FN) return cl->type.as.fn.result_kind;
    if (a->kind == EX_VAR && a->as.var.binding &&
        a->as.var.binding->type.kind == TY_FN)
        return a->as.var.binding->type.as.fn.result_kind;
    return TY_UNKNOWN;
}

void elab_track_bare_fat_lazy(Elab *e, Binding *b) {
    if (!b) return;
    for (uint32_t i = 0; i < e->n_bare_fat_lazy_bindings; i++)
        if (e->bare_fat_lazy_bindings[i] == b) return;
    if (e->n_bare_fat_lazy_bindings >= e->cap_bare_fat_lazy_bindings) {
        e->cap_bare_fat_lazy_bindings =
            e->cap_bare_fat_lazy_bindings ? e->cap_bare_fat_lazy_bindings * 2 : 8;
        e->bare_fat_lazy_bindings = (Binding **)realloc(
            e->bare_fat_lazy_bindings,
            e->cap_bare_fat_lazy_bindings * sizeof(Binding *));
    }
    e->bare_fat_lazy_bindings[e->n_bare_fat_lazy_bindings++] = b;
}

/* A short, C-identifier-safe suffix naming a result kind for a clone symbol.
 * Returns NULL for kinds we do not (yet) specialize over. */
static const char *bare_fat_kind_suffix(TypeKind k) {
    switch (k) {
        case TY_FLOAT:   return "float";
        case TY_FLOAT32: return "f32";
        case TY_FLOAT64: return "f64";
        default:         return NULL;
    }
}

Binding *elab_specialize_bare_fat(Elab *e, Binding *callee, TypeKind k) {
    if (!callee || !callee->defn_form) return NULL;
    const char *suffix = bare_fat_kind_suffix(k);
    if (!suffix) return NULL;

    /* A cache slot's spec field encodes three post-lookup states: NULL means a
     * specialization is *in progress* (a re-entry is therefore recursion);
     * BARE_FAT_FAILED means it already failed (its diagnostic was reported --
     * do not re-report); any other pointer is the finished clone. */
    static Binding BARE_FAT_FAILED_;
    Binding *const FAILED = &BARE_FAT_FAILED_;

    /* Dedup, and detect an in-progress (recursive) specialization. */
    for (uint32_t i = 0; i < e->n_bare_fat_specs; i++) {
        if (e->bare_fat_specs[i].orig == callee && e->bare_fat_specs[i].kind == k) {
            if (e->bare_fat_specs[i].spec == NULL) {
                diag_emit(DIAG_ERROR, callee->span,
                    "bare-^fat function '%s' cannot be specialized at a non-int "
                    "result kind through recursion (not yet supported)",
                    callee->name->name);
                return NULL;
            }
            if (e->bare_fat_specs[i].spec == FAILED) return NULL;
            return e->bare_fat_specs[i].spec;
        }
    }
    if (e->n_bare_fat_specs >= e->cap_bare_fat_specs) {
        e->cap_bare_fat_specs = e->cap_bare_fat_specs ? e->cap_bare_fat_specs * 2 : 8;
        e->bare_fat_specs = (struct BareFatSpec *)realloc(
            e->bare_fat_specs, e->cap_bare_fat_specs * sizeof(*e->bare_fat_specs));
    }
    uint32_t slot = e->n_bare_fat_specs++;
    e->bare_fat_specs[slot].orig = callee;
    e->bare_fat_specs[slot].kind = k;
    e->bare_fat_specs[slot].spec = NULL;  /* in-progress marker */

    char nbuf[256];
    snprintf(nbuf, sizeof(nbuf), "%s__bf_%s", callee->name->name, suffix);
    const Symbol *mname = symtab_intern(e->st, strslice(nbuf, (uint32_t)strlen(nbuf)));

    /* Re-elaborate the retained Form under the specialization context, at file
     * scope so the clone's params do not capture the caller's locals. */
    bool          sv_active = e->bare_fat_spec_active;
    TypeKind      sv_kind   = e->bare_fat_spec_kind;
    const Symbol *sv_name   = e->bare_fat_spec_name;
    Binding      *sv_result = e->bare_fat_spec_result;
    Scope        *sv_scope  = e->scope;

    e->bare_fat_spec_active = true;
    e->bare_fat_spec_kind   = k;
    e->bare_fat_spec_name   = mname;
    e->bare_fat_spec_result = NULL;
    e->scope = &e->global;

    Expr   *def  = elab_defn(e, callee->defn_form);
    Binding *spec = e->bare_fat_spec_result;

    e->bare_fat_spec_active = sv_active;
    e->bare_fat_spec_kind   = sv_kind;
    e->bare_fat_spec_name   = sv_name;
    e->bare_fat_spec_result = sv_result;
    e->scope = sv_scope;

    if (!def || !spec) {
        /* Mark finished-failed so a later identical call returns NULL silently
         * instead of tripping the in-progress (recursion) check above. */
        e->bare_fat_specs[slot].spec = FAILED;
        return NULL;
    }
    if (def->kind == EX_FN_DEF) elab_register_file_def(e, def);

    callee->bare_fat_specialized = true;
    spec->bare_fat_specialized   = true;
    e->bare_fat_specs[slot].spec = spec;
    return spec;
}

void elab_sweep_bare_fat_lazy(Elab *e) {
    for (uint32_t i = 0; i < e->n_bare_fat_lazy_bindings; i++) {
        Binding *b = e->bare_fat_lazy_bindings[i];
        if (!b || b->bare_fat_specialized || !b->defn_form) continue;
        /* Never specialized: re-run the canonical (int) body with capture OFF so
         * the real diagnostic that caused the deferral is surfaced. */
        e->bare_fat_force_canonical = true;
        (void)elab_defn(e, b->defn_form);
        e->bare_fat_force_canonical = false;
    }
}

/* constrained-generic-as-value-bakes-representative.md: eta-expand a BARE
 * constrained-generic global function passed where a concrete function type is
 * expected -- `(apply-fn count-it box)` with `apply-fn : (fn [(fn [Box] int) ..])`
 * -- into `(fn [g..] (count-it g..))`.  Passing the bare fn coerces it to the
 * carrier representative instance (a silent miscompile); the eta-expanded
 * lambda's body is a DIRECT call where the existing per-call-site generic-dict
 * specialization fires, and its un-annotated params are typed from `expected`
 * via bidirectional inference (see elab_fn).  Returns the elaborated lambda, or
 * NULL when not applicable (caller elaborates the arg normally).  Gated to the
 * dispatch-relevant case (a tyvar param pinned to a non-primitive concrete type)
 * and to simple fns (no substructural/variadic/fat/rank-2 params). */
static Expr *try_eta_expand_generic_fn_arg(Elab *e, const Form *arg_form,
                                           const Type *expected) {
    if (!arg_form || arg_form->tag != F_SYM) return NULL;
    if (!expected || expected->kind != TY_FN || !expected->as.fn.arg_full_types)
        return NULL;
    bool qerr = false;
    Binding *vb = elab_lookup_sym(e, arg_form->as.sym, arg_form->span, &qerr);
    if (!vb) return NULL;
    /* Follow a let-bound alias to its global (mirrors the call-head rule). */
    if (!vb->is_global && vb->source_binding && vb->source_binding->is_global)
        vb = vb->source_binding;
    if (!vb->is_global || vb->type.kind != TY_FN || vb->is_poly_fn) return NULL;
    const Type *vt = &vb->type;
    if (!vt->as.fn.arg_full_types) return NULL;
    uint32_t ar = vt->as.fn.arity;
    if (ar == 0 || ar > MAX_FN_ARITY || ar != expected->as.fn.arity) return NULL;
    /* poly-hof-reversed-order-primitive-pin: a "pin" is any generic parameter
     * slot (gt is a bare tyvar) whose expected counterpart `ct` is already
     * concrete -- not just a struct/ADT/type-app, but also a primitive such as
     * `int`.  When the value argument precedes the fn argument the HOF's tyvar
     * is pinned early via the sibling look-ahead, so by the time we get here
     * `ct` is the concrete `int`; eta-expanding then specializes the bare
     * constrained-generic global (e.g. `count-it`) to its real instance
     * (`Size[int]`) instead of leaving a bare `(fn [tyvar] int)` representative.
     * Abstract pins (`ct` still a tyvar, i.e. the relay path) are deliberately
     * excluded so the enclosing-generic case stays abstract. */
    bool pins_concrete = false;
    for (uint8_t k = 0; k < ar; k++) {
        if (vt->as.fn.arg_flags[k] != 0)  /* any per-arg ownership/cc marker set */
            return NULL;
        const Type *gt = vt->as.fn.arg_full_types[k];
        const Type *ct = expected->as.fn.arg_full_types[k];
        if (gt && gt->kind == TY_TYVAR && ct &&
            ct->kind != TY_TYVAR && ct->kind != TY_UNKNOWN)
            pins_concrete = true;
    }
    if (!pins_concrete) return NULL;
    /* Build (fn [g$eta$0 ...] (<name> g$eta$0 ...)). */
    Span sp = arg_form->span;
    Form **pvec   = (Form **)arena_alloc(e->arena, ar * sizeof(Form *));
    Form **bitems = (Form **)arena_alloc(e->arena, (ar + 1u) * sizeof(Form *));
    bitems[0] = form_sym(e->arena, sp, arg_form->as.sym);
    for (uint8_t k = 0; k < ar; k++) {
        char nm[32];
        int nl = snprintf(nm, sizeof(nm), "g$eta$%u", (unsigned)k);
        const Symbol *psym = symtab_intern(e->st, strslice(nm, (uint32_t)nl));
        pvec[k]       = form_sym(e->arena, sp, psym);
        bitems[k + 1] = form_sym(e->arena, sp, psym);
    }
    Form **fnitems = (Form **)arena_alloc(e->arena, 3 * sizeof(Form *));
    fnitems[0] = form_sym(e->arena, sp, e->sym_fn);
    fnitems[1] = form_vec(e->arena, sp, pvec, ar);
    fnitems[2] = form_list(e->arena, sp, bitems, ar + 1u);
    Form *eta = form_list(e->arena, sp, fnitems, 3);
    Type *saved = e->expected_type;
    e->expected_type = (Type *)expected;
    Expr *r = elab_form(e, eta);
    e->expected_type = saved;
    return r;
}

/* Bare-ctor expected type: is `f` literally a constructor call -- `(Empty)`,
 * `(none)`, `(Some x)` -- of the SAME ADT that `app` applies?
 *
 * A nullary constructor of a parametric ADT carries no field arguments to bind
 * its type parameters from, so it defaults to the bare TY_ADT and emits the
 * un-monomorphised carrier ctor.  The consumer that fixes this already exists
 * (see the "Parametric 0-arg constructor result-type inference" block), and the
 * ascription path `(:: (Empty) (Box int))` already feeds it -- but nothing fed
 * it from a call's ARGUMENT position, so `(getv (Empty))` against
 * `getv [b : (Box int)]` still picked `ctor_Empty()`.
 *
 * Gated on the ctor's own ADT matching the parameter's, so the pushed type can
 * only ever ground a constructor to a family it already belongs to. */
static bool arg_form_is_ctor_call_of(Elab *e, const Form *f, const Type *app) {
    if (!e || !f || !app || app->kind != TY_APP) return false;
    const Symbol *name = NULL;
    if (f->tag == F_LIST && f->as.list.len >= 1 &&
        f->as.list.items[0]->tag == F_SYM)
        name = f->as.list.items[0]->as.sym;
    if (!name) return false;
    CtorDef *c = elab_lookup_ctor(e, name);
    return c && c->adt && c->adt->n_type_params > 0 &&
           c->adt == type_adt_app_def(app);
}

/* Phase 2: Elaborate a function call (f a b c) */
/* any-struct-box-leak-per-widen: does `x` evaluate to an `any` whose payload box
 * THIS expression owns -- nothing else holds it, so the consumer may drop it?
 *
 * Two base cases and one step.  A widen performed here mints the box (a
 * frame-boxed one is excluded: that payload is a stack address).  A call to a
 * returns_fresh_any producer mints one per call.  And a call to a passthrough
 * (returns_any_param_idx) forwards its argument without keeping it, so the
 * question moves to that argument -- which is what makes `(alias tmp)` owned
 * when `tmp` was, and NOT owned when the caller was handed it.
 *
 * Depth-bounded rather than recursive-without-limit: the chain is a syntactic
 * one through named callees, so a handful of links is generous, and a bound
 * removes any question about a pathological program.  Running out of depth
 * answers "not owned", which only ever declines a drop. */
bool any_expr_is_owned_temp(const Expr *x, int depth) {
    if (!x || depth <= 0) return false;
    while (x && x->kind == EX_ASCRIBE) x = x->as.ascribe_.inner;
    if (!x || x->type.kind != TY_ANY) return false;
    /* union-to-any-widen-emits-uncompilable-c: widening a UNION to `any` mints
     * nothing -- it re-tags a `tur_tagged_t` whose box the union already owns --
     * so this expression never owns that payload and a consumer must not drop it.
     *
     * Today the guard below would also decline, because the frame-box rule at the
     * call site carries the SAME three conditions and sets `frame_box` first. That
     * is a coincidence of two independent rules, not a reason: relax either guard
     * and a consumer starts freeing a box the union still holds. Say it directly.
     * The sibling hole in `let_binding_any_freeable` was not so lucky -- it fired,
     * and printed garbage out of a freed box. */
    if (x->kind == EX_UNION_INJECT && x->as.union_inject_.value &&
        x->as.union_inject_.value->type.kind == TY_UNION)
        return false;
    if (x->kind == EX_UNION_INJECT) return !x->as.union_inject_.frame_box;
    if (x->kind != EX_CALL || !x->as.call_.fn_binding) return false;
    const Binding *fb = x->as.call_.fn_binding;
    if (fb->returns_fresh_any) return true;
    int pi = fb->returns_any_param_idx;
    if (pi >= 0 && (uint32_t)pi < x->as.call_.n_args && x->as.call_.args)
        return any_expr_is_owned_temp(x->as.call_.args[pi], depth - 1);
    return false;
}

/* erased-closure-param-over-niche-vec-slot-reads-box.
 *
 * Two stdlib helpers hand a user comparator a RAW container word rather than a
 * value that has crossed the carrier boundary: `vec-eq?`, whose inline-C loop
 * passes `a->data[i]` straight through, and `list-eq?`, which passes
 * `(list-head l)` -- the cons cell's stored carrier. For a niche `(Option P)`
 * that word is the payload pointer ITSELF (container-element-form plan, CE2),
 * so a comparator that ascribes its parameter back to the element type must
 * REINTERPRET the word; unboxing it as a carrier box reads the payload's own
 * first words as a tag and a value and answers silently wrong.
 *
 * The synthesized comparator (elab_typeclasses.c build_comparator_lambda) says
 * this by naming its parameters `__cmp_slot_*` -- the prefix emit_slot_word_is
 * recognises.  A user's lambda in that position is the same value reached the
 * same way, so it earns the same mark.  Keep this predicate in step with
 * build_comparator_lambda's `strcmp(sd->name, "Vec") == 0`, which is the
 * synthesized half of the same fact.
 *
 * `map-eq?` and `set-eq-cmp?` are NOT here and must not be: they iterate a HAMT
 * and hand over the stored value/key, which IS a carrier box, so their erased
 * comparators are already right.  (Their TYPED comparators are wrong for the
 * mirror-image reason -- filed separately as
 * docs/reported/typed-comparator-over-hamt-box-compares-box-addresses.md.) */
static bool call_comparator_gets_slot_words(const Binding *fn_binding,
                                            uint32_t argi) {
    if (!fn_binding || !fn_binding->name || !fn_binding->name->name) return false;
    const char *n = fn_binding->name->name;
    /* `vec-eq?` passes `a->data[i]`, `list-eq?` passes `(list-head l)`, and
     * `result-eq?` passes the sum's payload slot -- all three the stored word.
     * result-eq? has TWO comparators, one per arm, and both are fed payloads. */
    if (argi == 2 && (strcmp(n, "vec-eq?") == 0 || strcmp(n, "list-eq?") == 0))
        return true;
    return (argi == 2 || argi == 3) && strcmp(n, "result-eq?") == 0;
}

/* The other half of the same fact: `map-eq?` / `map-eq-raw-k?` /
 * `set-eq-cmp?` iterate a HAMT and hand the comparator the stored value or key
 * VERBATIM, and a HAMT stores a niche `(Option P)` as a carrier box rather than
 * as the payload word.  So a comparator here is in the mirror-image position of
 * one at `vec-eq?`: its ERASED parameter is already right (the ascription
 * unboxes a box that really is there), and its parameter DECLARED as the
 * element type is the broken one -- it is emitted as the niche pointer and
 * receives a box address, so a value compares unequal to itself.
 *
 * That the HAMT boxes where a Vec slot does not is CE4, still deferred
 * (docs/archive/container-element-form-plan.md).  This rule does not decide
 * that question; it only makes the read side agree with whatever the container
 * currently stores. */
static bool call_comparator_gets_carrier_box(const Binding *fn_binding,
                                             uint32_t argi) {
    if (!fn_binding || !fn_binding->name || !fn_binding->name->name) return false;
    const char *n = fn_binding->name->name;
    if (argi == 2 && (strcmp(n, "map-eq?") == 0 || strcmp(n, "set-eq-cmp?") == 0))
        return true;
    return argi == 3 && strcmp(n, "map-eq-raw-k?") == 0;
}

/* Mark a comparator lambda's parameters that are DECLARED as a niche option, so
 * emit unboxes the carrier at body entry.  Returns true when the argument was a
 * lambda literal (markable) -- a name cannot carry the mark for the same reason
 * it cannot carry the slot-word one: the same function may be called elsewhere
 * with a value that really is the niche word. */
static bool mark_comparator_carrier_box_params(Expr *arg) {
    Expr *inner = arg;
    while (inner && (inner->kind == EX_ASCRIBE || inner->kind == EX_FN_TO_FAT))
        inner = (inner->kind == EX_ASCRIBE) ? inner->as.ascribe_.inner
                                            : inner->as.fn_to_fat_.inner;
    if (!inner) return false;
    FnDef *fd = NULL;
    if (inner->kind == EX_CLOSURE && inner->as.closure_.closure)
        fd = inner->as.closure_.closure->fn;
    else if (inner->kind == EX_VAR && inner->as.var.binding &&
             inner->as.var.binding->is_lifted_lambda)
        fd = inner->as.var.binding->source_fn_def;
    if (!fd || !fd->params) return false;
    if (fd->body && fd->body->kind == EX_INLINE_C) return true;
    for (uint32_t p = 0; p < fd->n_params; p++) {
        Binding *pb = fd->params[p];
        if (!pb) continue;
        /* Only a parameter declared AS the element type is affected.  An erased
         * (`int`) parameter is bridged by its own ascription and is correct. */
        if (adt_app_is_niche_option(pb->type)) pb->arrives_as_carrier_box = true;
    }
    return true;
}

/* The element type of a `(Vec E)` argument, peeled back through whatever
 * coercion the carrier-typed `v1 : int` parameter wrapped it in.  Returns a
 * TY_NIL type when the argument is not a Vec application. */
static Type comparator_vec_elem_type(const Expr *a) {
    while (a && (a->kind == EX_ASCRIBE || a->kind == EX_REINTERPRET)) {
        if (a->type.kind == TY_APP) break;
        a = (a->kind == EX_ASCRIBE) ? a->as.ascribe_.inner
                                    : a->as.reinterpret_.expr;
    }
    if (a && a->type.kind == TY_APP && a->type.as.app.arg)
        return *a->type.as.app.arg;
    return TYPE_NIL;
}

/* Mark a comparator LAMBDA's parameters as raw slot words, by renaming them
 * into the `__cmp_slot_` namespace.  The name is the whole mechanism: emit
 * derives the C parameter name from `binding->name` at the declaration and at
 * every use, so definition and uses move together, and the mark is
 * self-scoping -- unlike the value table, which is program-wide and would make
 * every unrelated local named `a` look like a slot word.
 *
 * Unconditional for a lambda in this position, and ACCURATE rather than merely
 * inert for a non-niche element: a `vec-eq?` comparator parameter is a raw slot
 * word whatever the element type is.  The mark is consulted by exactly one
 * carrier->concrete bridge row, whose sink form must be CE_WORD, so every other
 * element type reaches byte-identical code.
 *
 * Returns true when the argument was a lambda literal (markable), false when it
 * is a name -- a named function is elaborated once and may also be called with
 * genuine boxes, so it cannot carry this mark and gets the diagnostic instead. */
/* Rename one parameter into the `__cmp_slot_` namespace.  The MARK IS THE
 * EMITTED NAME, so it has to survive name mangling intact: a pure C identifier
 * is emitted verbatim (raw_name_for_binding passes reserved-prefix identifiers
 * through), while anything carrying a kebab or sigil byte would go through the
 * injective mangler, which rewrites the prefix itself and silently loses the
 * mark -- those fall back to the parameter index.  A parameter already in the
 * reserved `__` namespace is skipped: that covers a lifted closure's env
 * pointer and an already-marked parameter, and users cannot write such a name. */
static void mark_one_param_as_slot_word(Elab *e, Binding *pb, uint32_t idx) {
    if (!pb || !pb->name || !pb->name->name) return;
    if (pb->name->name[0] == '_' && pb->name->name[1] == '_') return;
    bool pure = true;
    for (const char *c = pb->name->name; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_')) { pure = false; break; }
    char buf[256];
    if (pure) snprintf(buf, sizeof buf, "__cmp_slot_%s", pb->name->name);
    else      snprintf(buf, sizeof buf, "__cmp_slot_p%u", (unsigned)idx);
    pb->name = intern_cstr(e->st, buf);
}

static bool mark_comparator_slot_word_params(Elab *e, Expr *arg) {
    Expr *inner = arg;
    while (inner && (inner->kind == EX_ASCRIBE || inner->kind == EX_FN_TO_FAT))
        inner = (inner->kind == EX_ASCRIBE) ? inner->as.ascribe_.inner
                                            : inner->as.fn_to_fat_.inner;
    if (!inner) return false;
    FnDef *fd = NULL;
    if (inner->kind == EX_CLOSURE && inner->as.closure_.closure) {
        /* A CAPTURING lambda written at the call. */
        fd = inner->as.closure_.closure->fn;
    } else if (inner->kind == EX_VAR && inner->as.var.binding &&
               inner->as.var.binding->is_lifted_lambda) {
        /* A CAPTURELESS lambda written at the call: elab_fn lifts it to a
         * file-scope `__fn_N` and hands back a reference to it.  The lift is
         * what makes this shape look like a name; `is_lifted_lambda` is what
         * separates it from one.  The binding is minted for this argument and
         * reachable from nowhere else, so marking its parameters cannot reach
         * another caller -- which is exactly what a real name cannot promise. */
        fd = inner->as.var.binding->source_fn_def;
    }
    if (!fd || !fd->params) return false;
    /* An inline-C body names its parameters by their source spelling, so a
     * rename would desync the body text from the emitted signature. */
    if (fd->body && fd->body->kind == EX_INLINE_C) return true;
    for (uint32_t p = 0; p < fd->n_params; p++)
        mark_one_param_as_slot_word(e, fd->params[p], p);
    return true;
}

/* Peel a call argument or call head down to the lambda literal behind it, or
 * NULL when it is a name.  Shared by the two sites that mark parameters. */
static FnDef *lambda_literal_fn_def(Expr *x) {
    while (x && (x->kind == EX_ASCRIBE || x->kind == EX_FN_TO_FAT))
        x = (x->kind == EX_ASCRIBE) ? x->as.ascribe_.inner
                                    : x->as.fn_to_fat_.inner;
    if (!x) return NULL;
    if (x->kind == EX_CLOSURE && x->as.closure_.closure)
        return x->as.closure_.closure->fn;
    if (x->kind == EX_VAR && x->as.var.binding &&
        x->as.var.binding->is_lifted_lambda)
        return x->as.var.binding->source_fn_def;
    return NULL;
}

/* pair-eq-macro-applies-comparator-directly.
 *
 * The third way a lambda meets a niche word, and the one neither call-keyed
 * rule could reach: it is APPLIED DIRECTLY, with no named callee whose
 * parameter contract could say which convention the value arrives in.
 * `pair-eq?` is a macro that splices its comparator into call position against
 * a field read --
 *
 *   `(~fst-cmp (.fst ~p1) (.fst ~p2))`
 *
 * -- and `.fst` on a niche `(Option P)` field yields the payload pointer, so an
 * erased parameter's `(:: a (Option P))` unboxed a box that was never there.
 *
 * But a direct application needs no contract, because the ARGUMENT is right
 * here: this is the one site where the convention is read off the value being
 * passed rather than promised by a callee.  A concrete niche-typed argument IS
 * the word (that is what the representation means), so an erased parameter
 * receiving one is receiving a word.  Per PARAMETER, not per lambda -- a
 * comparator may take a niche argument in one position and an int in another,
 * as `pair-eq?`'s own two lambdas do.
 *
 * Only an ERASED parameter is touched.  One declared as the element type has
 * no bridge to redirect and is already correct. */
static void mark_direct_apply_niche_word_params(Elab *e, Expr *head_expr,
                                                Expr *call_expr) {
    if (!call_expr || call_expr->kind != EX_CALL) return;
    FnDef *fd = lambda_literal_fn_def(head_expr);
    if (!fd || !fd->params) return;
    if (fd->body && fd->body->kind == EX_INLINE_C) return;
    uint32_t n_args = call_expr->as.call_.n_args;
    /* A lifted closure thunk carries its env as params[0], so the parameter
     * vector can be one longer than the argument list. */
    uint32_t off = (fd->n_params == n_args + 1) ? 1u : 0u;
    if (fd->n_params != n_args + off) return;
    for (uint32_t i = 0; i < n_args; i++) {
        Expr *a = call_expr->as.call_.args[i];
        Binding *pb = fd->params[i + off];
        if (!a || !pb) continue;
        if (pb->type.kind != TY_INT) continue;      /* erased only */
        if (!adt_app_is_niche_option(a->type)) continue;
        mark_one_param_as_slot_word(e, pb, i + off);
    }
}

/* r7rs-too-few-arguments-returns-a-procedure: the error a Scheme call with
 * too few arguments raises, as `(do arg... (r7rs-signal__ "msg"))` -- the
 * arguments still run, in order, and the error object carries a message
 * naming the procedure the way the program spelled it.  NULL outside user
 * Scheme source, or when the prelude's raiser is not in scope; the caller
 * then builds the partial application as before.
 *
 * r7rs-dead-mistyped-call-refused-at-compile-time: and a call with too MANY
 * arguments to a fixed-arity procedure, which Turmeric reads as applying the
 * result to the rest (and refuses when the result is not a function).  R7RS
 * makes it an error only when it runs, so it raises the same way. */
static Expr *scheme_arity_error(Elab *e, const Form *call, const Binding *fn_binding,
                                uint32_t n_args, uint32_t n_required, bool variadic) {
    if (!scheme_span_is_user_source(call->span) || !fn_binding || !fn_binding->name)
        return NULL;
    const Symbol *sig = symtab_intern(e->st, strslice("r7rs-signal__", 13));
    bool qual_err = false;
    Binding *sb = elab_lookup_sym(e, sig, call->span, &qual_err);
    if (!sb || sb->type.kind != TY_FN) return NULL;
    char nbuf[256];
    const char *pub = scheme_source_name(fn_binding->name->name, nbuf, sizeof nbuf);
    char msg[320];
    snprintf(msg, sizeof msg, "%s: too %s arguments (expects %s%u, got %u)", pub,
             n_args < n_required ? "few" : "many",
             variadic ? "at least " : "", n_required, n_args);
    size_t ml = strlen(msg);
    char *mp = (char *)arena_alloc(e->arena, ml + 1);
    memcpy(mp, msg, ml + 1);
    Form **items = (Form **)arena_alloc(e->arena, (n_args + 2) * sizeof(Form *));
    items[0] = form_sym(e->arena, call->span, symtab_intern(e->st, strslice("do", 2)));
    for (uint32_t i = 0; i < n_args; i++) items[1 + i] = call->as.list.items[1 + i];
    Form **sc = (Form **)arena_alloc(e->arena, 2 * sizeof(Form *));
    sc[0] = form_sym(e->arena, call->span, sig);
    sc[1] = form_str(e->arena, call->span, mp, (uint32_t)ml);
    items[1 + n_args] = form_list(e->arena, call->span, sc, 2);
    return elab_form(e, form_list(e->arena, call->span, items, n_args + 2));
}

/* associated-type-unusable-nullary-and-generic (half 2): the unreduced
 * projections (`(Inner A)`, types.h tyvar_.assoc_of) a callee's signature
 * mentions. */
static void call_collect_assoc_projs(const Type *t, const Type **out,
                                     uint8_t *n, uint8_t cap) {
    if (!t || *n >= cap) return;
    switch (t->kind) {
        case TY_TYVAR:
            if (t->as.tyvar_.assoc_of && t->as.tyvar_.name) {
                for (uint8_t i = 0; i < *n; i++)
                    if (out[i]->as.tyvar_.name == t->as.tyvar_.name) return;
                out[(*n)++] = t;
            }
            return;
        case TY_APP:
            call_collect_assoc_projs(t->as.app.fn, out, n, cap);
            call_collect_assoc_projs(t->as.app.arg, out, n, cap);
            return;
        case TY_FN:
            if (t->as.fn.arg_full_types)
                for (uint32_t i = 0; i < t->as.fn.arity; i++)
                    call_collect_assoc_projs(t->as.fn.arg_full_types[i], out, n, cap);
            call_collect_assoc_projs(t->as.fn.result_full_type, out, n, cap);
            return;
        default:
            return;
    }
}

/* Reduce each projection the callee's signature mentions against this call's
 * bindings, once every argument is in.  `(Inner A)` with A fixed to a ground
 * type is the instance's answer -- an argument typed `(Inner A)` must agree
 * with it, and the binding it adds (by the projection's name) is what
 * instantiates the result and what the emitter's per-call spec substitutes, so
 * a by-value associated type gets its real C type in the clone.  At another
 * type variable (`A := B` inside a generic caller) it becomes `(Inner B)`.
 * Returns false after reporting. */
static bool call_bind_assoc_projections(Elab *e, const Form *call,
                                        const Binding *fn_binding,
                                        const Type *fn_type,
                                        CallTypeBinding *bindings,
                                        uint8_t *n_bindings) {
    if (!fn_type || fn_type->kind != TY_FN) return true;
    const Type *projs[16];
    uint8_t np = 0;
    call_collect_assoc_projs(fn_type, projs, &np, 16);
    for (uint8_t pi = 0; pi < np; pi++) {
        const Type *P = projs[pi];
        Type a = call_instantiate_type(e, P->as.tyvar_.assoc_arg, bindings, *n_bindings);
        Type want;
        if (!call_type_has_named_tyvar(&a)) {
            const Type *r = typeclass_env_resolve_assoc_type_n(
                &e->typeclass_env, P->as.tyvar_.assoc_of, &a, 1);
            if (!r) {
                Buf ab; buf_init(&ab);
                type_print(&ab, a);
                buf_putc(&ab, '\0');
                diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0015_TYPECLASS_CONSTRAINT_NOT_SATISFIED,
                    "function '%s': no instance binding for associated type '%s' "
                    "at %s (its signature mentions %s)",
                    fn_binding && fn_binding->name ? fn_binding->name->name : "?",
                    P->as.tyvar_.assoc_of->name, ab.data, P->as.tyvar_.name);
                buf_free(&ab);
                return false;
            }
            want = *r;
        } else if (type_eq(a, *P->as.tyvar_.assoc_arg)) {
            continue;   /* the variable stands for itself (a recursive call) */
        } else if (!elab_assoc_projection(e, P->as.tyvar_.assoc_of, &a, 1, &want)) {
            continue;
        }
        uint8_t idx = 0;
        if (call_find_type_binding(bindings, *n_bindings, P->as.tyvar_.name, &idx)) {
            if (!type_eq(bindings[idx].type, want)) {
                Buf wb; buf_init(&wb);
                type_print(&wb, want);
                buf_putc(&wb, '\0');
                Buf gb; buf_init(&gb);
                type_print(&gb, bindings[idx].type);
                buf_putc(&gb, '\0');
                diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0001_TYPE_MISMATCH,
                    "function '%s': %s is %s here, but an argument gives it %s",
                    fn_binding && fn_binding->name ? fn_binding->name->name : "?",
                    P->as.tyvar_.name, wb.data, gb.data);
                buf_free(&wb);
                buf_free(&gb);
                return false;
            }
            bindings[idx].type = want;
            continue;
        }
        if (*n_bindings >= 16) return true;
        bindings[*n_bindings].name = P->as.tyvar_.name;
        bindings[*n_bindings].type = want;
        (*n_bindings)++;
    }
    return true;
}

static Expr *elab_call_fn_inner(Elab *e, const Form *call, Binding *fn_binding) {
    uint32_t n_args = call->as.list.len - 1;

    /* defstruct-as-defadt (exg5-exists-cycle): read-and-clear the make-struct
     * leniency flag at entry.  It is set by the make-struct -> ctor-call rewrite
     * (elab_structs.c) ONLY for a non-parametric record ADT, so the ctor call's
     * own positional args relax to default make-struct's no-field-typecheck
     * parity (e.g. a `0`/NULL ptr<void> for an rc<T>/ptr<T> field).  Clearing it
     * here means the args elaborated below -- and any nested calls they spawn --
     * do not inherit the leniency; only THIS call's direct args are relaxed. */
    bool ms_lenient = e->make_struct_lenient_args;
    e->make_struct_lenient_args = false;

    /* Get the function type */
    Type fn_type = fn_binding->type;
    
    /* Phase HRT1: rank-2 polymorphic function parameter call — intercept before closure/PTR_VOID. */
    if (fn_binding->is_poly_fn) {
        return elab_poly_call(e, call, fn_binding);
    }

    /* For closure bindings, use the closure's thunk function type */
    if (fn_binding->closure_fn_binding) {
        /* This is a closure - get the thunk function type */
        fn_type = fn_binding->closure_fn_binding->type;
        /* poly-combinator-application-element-inference: the closure came from
         * applying a polymorphic combinator (e.g. `(or-parser af ao)` returning
         * `(fn [int] (PRes A))`).  The call site already grounded that
         * application's element tyvar and recorded the result on the LET
         * binding's OWN type (`combined : (fn [int] (PRes int))`), but the
         * shared closure thunk binding still carries the ungrounded `(PRes A)`.
         * Taking the thunk type verbatim reintroduces the bare tyvar, so
         * `(combined 7)` types as `(PRes A)` and the downstream `match` rejects
         * its arms.  When the binding's own type is a fully-ground TY_FN whose
         * result refines the thunk's tyvar-bearing result, unify the two result
         * types (`(PRes A)` vs `(PRes int)` -> `A = int`) and substitute so the
         * call's result type grounds. */
        /* ...and when that result names only the ENCLOSING signature's own
         * variables (`f : (fn [] A)` from `(mk x)`, `x : A`, where the thunk
         * says `(fn [] B)`): those are as fixed as a concrete type in each
         * instantiation.  Left as `B`, a callee tyvar the enclosing body
         * cannot resolve, `(f)` fell back to the carrier `int`, and `(pair (f)
         * 0)` minted `(Pair int int)` in every spec -- invalid C at a float
         * (found by the type fuzzer's gbody crossing). */
        if (fn_type.kind == TY_FN && fn_type.as.fn.result_full_type &&
            call_type_has_named_tyvar(fn_type.as.fn.result_full_type) &&
            fn_binding->type.kind == TY_FN &&
            fn_binding->type.as.fn.result_full_type &&
            (!call_type_has_named_tyvar(fn_binding->type.as.fn.result_full_type) ||
             call_type_tyvars_all_in_sig(e, fn_binding->type.as.fn.result_full_type))) {
            CallTypeBinding cbind[16];
            uint8_t n_cbind = 0;
            if (call_collect_type_bindings(fn_type.as.fn.result_full_type,
                                           *fn_binding->type.as.fn.result_full_type,
                                           cbind, &n_cbind) && n_cbind > 0) {
                fn_type = call_instantiate_type(e, &fn_type, cbind, n_cbind);
            }
        }
        /* generic-closure-return-type-app (Defect A): the thunk's
         * result_full_type is NULL -- the "(type-app ? ?)" shell the lambda
         * grounding gate deliberately leaves when the inferred body type
         * mentions the enclosing fn's tyvar (elab_fns.c, closure-result-
         * monomorphization Phase 2).  The recovery above is guarded on it
         * being non-NULL, so it never fired and the call's result fell
         * through to `type_from_kind(TY_APP)`: a zeroed app whose printer
         * renders `(type-app ? ?)`.  But the BINDING's own type carries the
         * correctly-instantiated result -- elab_call_head_expr copied the
         * head expr's `(fn [] (Cons int))` onto the temp, and a user let
         * records the grounded type the same way -- so graft that result
         * onto the thunk signature.  Result slot only: the thunk's arg list
         * includes the hidden env parameter and must not be replaced.  This
         * is the report's "narrower change": the deliberate grounding
         * invariant in elab_fns.c is untouched. */
        if (fn_type.kind == TY_FN && !fn_type.as.fn.result_full_type &&
            (fn_type.as.fn.result_kind == TY_APP ||
             fn_type.as.fn.result_kind == TY_ADT) &&
            fn_binding->type.kind == TY_FN &&
            fn_binding->type.as.fn.result_full_type &&
            fn_binding->type.as.fn.result_full_type->kind ==
                fn_type.as.fn.result_kind &&
            !call_type_has_named_tyvar(
                fn_binding->type.as.fn.result_full_type)) {
            Type *ground = (Type *)arena_alloc(e->arena, sizeof(Type));
            *ground = *fn_binding->type.as.fn.result_full_type;
            fn_type.as.fn.result_full_type = ground;
        }
    } else if (fn_binding->type.kind == TY_PTR_VOID && !fn_binding->is_fat) {
        /* CRU B-4: retire the :ptr<void>-as-closure overload.  A *raw*
         * :ptr<void> (not a fat sink) is a plain pointer, not a callable
         * closure -- calling it directly is the representation-split hazard
         * (report ptr-void-direct-call-representation-split).  Closures are now
         * boxed TY_FN (B-1); a fat-closure parameter must be spelled ^fat (or
         * ^fat :(fn [...] :T)).  This makes :ptr<void> raw-pointer-only again. */
        diag_emit(DIAG_ERROR, call->span,
                  "'%s' has type :ptr<void> (a raw pointer), which is not "
                  "directly callable; declare it as a fat closure parameter "
                  "(^fat %s, or ^fat %s :(fn [...] :T)) to call it",
                  fn_binding->name->name, fn_binding->name->name,
                  fn_binding->name->name);
        return NULL;
    } else if (fn_binding->type.kind == TY_PTR_VOID) {
        /* CY2: fat-closure dynamic dispatch through a ^fat :ptr<void> sink.
         * Supports 0-arg and n-arg fat-closure calls (emit_expr.c reads slot 0
         * of the box for all arities). */
        Expr **cb_args = NULL;
        if (n_args > 0) {
            cb_args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
            for (uint32_t i = 0; i < n_args; i++) {
                cb_args[i] = elab_form(e, call->as.list.items[1 + i]);
                if (!cb_args[i]) return NULL;
            }
        }
        /* bare-fat-result-monomorphization (Phase B): type `(g x)` from the
         * bare-^fat param's recorded result kind.  TY_UNKNOWN (the zero default,
         * i.e. the canonical body) falls back to the int64 carrier -- identical
         * to the prior always-int behavior; a specialized clone stamps the
         * incoming closure's kind (e.g. TY_FLOAT) so the result lands in the
         * right register in ANY position, not just the tail Phase A retypes. */
        TypeKind rk = (fn_binding->bare_fat_result_kind != TY_UNKNOWN)
                    ? fn_binding->bare_fat_result_kind : TY_INT;
        Expr *out = expr_new(e->arena, EX_CALL, type_from_kind(rk), call->span);
        out->as.call_.fn_binding = fn_binding;
        out->as.call_.args = cb_args;
        out->as.call_.n_args = n_args;
        out->as.call_.fn_expr = NULL;
        out->as.call_.dict_arg = NULL;
        out->as.call_.is_poly_call = false;
        out->as.call_.poly_arg_mask = 0;
        return out;
    }
    
    /* (k v) application sugar for an EFFECT handler continuation (bound by a
     * `handle` case; `is_continuation`, resumed via the `resume` special form).
     * Unlike a cloneable continuation (TY_CONT, below), a handler continuation is
     * carried as a plain int64 and resumed by EX_RESUME -- so route `(k v)` to
     * `resume`, giving handler and cloneable continuations one uniform `(k v)`
     * spelling.  This also lets a receiver `(fn [k] (k v))` be applied to a
     * handler continuation, which is the foundation for the shift/reset ->
     * synthetic-effect desugar (cross-function resume). */
    if (fn_type.kind != TY_FN && fn_type.kind != TY_CONT
        && fn_binding->is_continuation) {
        if (n_args != 1) {
            diag_emit(DIAG_ERROR, call->span,
                      "continuation '%s' takes exactly one argument (the resume value)",
                      fn_binding->name->name);
            return NULL;
        }
        if (cont_check_double_use(e, call->as.list.items[0])) return NULL;
        Expr *value = elab_form(e, call->as.list.items[1]);
        if (!value) return NULL;
        Expr *kvar = expr_new(e->arena, EX_VAR, fn_binding->type, call->span);
        kvar->as.var.binding = fn_binding;
        return elab_make_resume(e, kvar, value, call->span);
    }

    /* saffron-lang-plan S4/D4 (G5): a dynamic callee.  In a Saffron file a
     * binding of type `any` may hold a function, and calling it is the ordinary
     * case rather than an error -- higher-order code is the whole point of the
     * surface syntax.  Resolution moves to runtime, where the value's own tag
     * says whether it is callable and with what arity. */
    if (fn_type.kind == TY_ANY && lang_span_is_dynamic(call->span)) {
        Expr *fnv = expr_new(e->arena, EX_VAR, fn_binding->type, call->span);
        fnv->as.var.binding = fn_binding;
        return saffron_dyn_call_on(e, call, fnv);
    }

    if (fn_type.kind != TY_FN && fn_type.kind != TY_CONT) {
        diag_emit(DIAG_ERROR, call->span,
                  "'%s' is not a function or continuation", fn_binding->name->name);
        return NULL;
    }

    /* CC4 (cps-transform-plan): (k v) application sugar for a cloneable
     * continuation. A call through a cont-typed binding desugars to a resume of
     * the cloneable continuation handle -- what the surface previously had to
     * spell as (tur_cloneable_cont_resume k v). The handle is carried as an
     * int64_t (see type_c_name TY_CONT), so the resume builtin consumes it
     * directly. */
    if (fn_type.kind == TY_CONT) {
        if (n_args != 1) {
            diag_emit(DIAG_ERROR, call->span,
                      "continuation '%s' takes exactly one argument (the resume value)",
                      fn_binding->name->name);
            return NULL;
        }
        /* CONT_EFFECT (cross-function resume): the cont is an algebraic-effect
         * handler continuation carried as a plain int64.  `(k v)` resumes it via
         * EX_RESUME (dk_invoke), NOT a cloneable/escape/serial resume builtin --
         * exactly the `is_continuation`-binding path above, but reached through a
         * TY_CONT-typed receiver param that the __Shift desugar flavored
         * CONT_EFFECT.  This is what lets a receiver `(fn [k : effect-cont] (k v))`
         * resume the delimited continuation the enclosing reset's handler carries. */
        if ((ContFlavor)fn_type.as.cont.flavor == CONT_EFFECT) {
            if (cont_check_double_use(e, call->as.list.items[0])) return NULL;
            Expr *value = elab_form(e, call->as.list.items[1]);
            if (!value) return NULL;
            /* The continuation is carried as an int64 handle; emit_effects_resume
             * takes the real fiber-resume path (tur_effect_cont_resume) only for a
             * TY_INT-typed k -- any other kind falls through to the identity
             * "return the value" fallback.  So view the cont binding as its int64
             * carrier here (mirrors the handler-continuation `is_continuation`
             * path above and the cloneable-cont carrier view below).  Carry the
             * binding's copy_kind onto the carrier so a CK_MULTISHOT continuation
             * (multishot-effect-cont) takes the snapshot resume path -- resume
             * dispatches on the k EXPR's copy_kind, not the binding's. */
            Type kcar = TYPE_INT;
            kcar.copy_kind = fn_type.copy_kind;
            Expr *kvar = expr_new(e->arena, EX_VAR, kcar, call->span);
            kvar->as.var.binding = fn_binding;
            return elab_make_resume(e, kvar, value, call->span);
        }
        Expr *karg = elab_form(e, call->as.list.items[1]);
        if (!karg) return NULL;
        /* slice 4 (resuming-shift plan): for a fully-typed continuation
         * `Cont<BodyT,ResetT>` = (cont BodyT ResetT), the resume value must have
         * type BodyT.  An untyped-arg cont (the one-arg `(cont R)` / bare `cont`
         * spellings, arg == TY_UNKNOWN) stays unchecked, as before. */
        /* An `any`-typed continuation (the `: any` call/cc receiver) takes
         * any value: widen rather than reject, exactly as an `any` parameter
         * would. */
        if (fn_type.as.cont.arg == TY_ANY && karg->type.kind != TY_ANY &&
            karg->type.kind != TY_NEVER)
            karg = elab_coerce_to_any(e, karg);
        if (fn_type.as.cont.arg != TY_UNKNOWN
            && karg->type.kind != TY_UNKNOWN
            && karg->type.kind != fn_type.as.cont.arg) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0001_TYPE_MISMATCH,
                                "continuation '%s' expects a resume value of type %s, got %s",
                                fn_binding->name->name,
                                type_name(type_from_kind(fn_type.as.cont.arg)),
                                type_name(type_from_kind(karg->type.kind)));
            return NULL;
        }
        /* CC4.4: (k v) consumes the continuation.  This sugar builds the EX_VAR
         * by hand (below), bypassing the shared var-use consumption path, so
         * account for linearity here: invoking a ^linear k marks it consumed, and
         * a second invocation is a use-after-consume (TUR-E0101). */
        if (fn_binding->is_linear) {
            if (fn_binding->is_linear_consumed) {
                diag_emit_with_code(DIAG_ERROR, call->span,
                                    TUR_E0101_LINEAR_USE_AFTER_CONSUME,
                                    "linear value '%s' used after being consumed",
                                    fn_binding->name->name);
                return NULL;
            }
            fn_binding->is_linear_consumed = true;
        }
        /* The handle, viewed as its int64 carrier so the resume builtin types. */
        Expr *kvar = expr_new(e->arena, EX_VAR, TYPE_INT, call->span);
        kvar->as.var.binding = fn_binding;
        /* CC4: dispatch to the resume runtime selected by the cont flavor. */
        const char *resume_name;
        switch ((ContFlavor)fn_type.as.cont.flavor) {
            case CONT_ESCAPE: resume_name = "tur_escape_resume"; break;
            case CONT_SERIAL: resume_name = "tur_serial_cont_resume"; break;
            case CONT_CLONEABLE:
            default:          resume_name = "tur_cloneable_cont_resume"; break;
        }
        const BuiltinSpec *rspec =
            builtin_first_with_name(intern_cstr(e->st, resume_name));
        if (!rspec) {
            diag_emit(DIAG_ERROR, call->span,
                      "internal: continuation resume builtin missing");
            return NULL;
        }
        TypeKind res_kind = (fn_type.as.cont.returns != TY_UNKNOWN)
                            ? fn_type.as.cont.returns : TY_INT;
        /* The resume builtins are int64-carried. For a value-typed cont<T> with
         * T != int (e.g. cstr), bit-cast the resume value into the int carrier on
         * the way in and bit-cast the int result back to T on the way out, so the
         * emitted C is clean (no -Wint-conversion). */
        Expr *karg_c;
        if (karg->type.kind == TY_ANY) {
            /* A box does not fit the int64 `result` slot: hand over a heap
             * copy's address (__tur_escape_box_any), which the landing reads
             * and frees.  Only an escape continuation can be `any`-typed here
             * (the call/cc receiver rewrite is what mints one). */
            const BuiltinSpec *bspec =
                builtin_first_with_name(intern_cstr(e->st, "__tur_escape_box_any"));
            if (!bspec) {
                diag_emit(DIAG_ERROR, call->span,
                          "internal: escape box builtin missing");
                return NULL;
            }
            Expr **xargs = (Expr **)arena_alloc(e->arena, sizeof(Expr *));
            xargs[0] = karg;
            karg_c = expr_new(e->arena, EX_BUILTIN, TYPE_INT, call->span);
            karg_c->as.builtin.spec = bspec;
            karg_c->as.builtin.args = xargs;
            karg_c->as.builtin.n = 1;
        } else {
            karg_c = call_wrap_reinterpret(e, karg, TY_INT, call->span);
        }
        Expr **bargs = (Expr **)arena_alloc(e->arena, 2 * sizeof(Expr *));
        bargs[0] = kvar;
        bargs[1] = karg_c;
        Expr *out = expr_new(e->arena, EX_BUILTIN, TYPE_INT, call->span);
        out->as.builtin.spec = rspec;
        out->as.builtin.args = bargs;
        out->as.builtin.n = 2;
        /* An `any` result is not reinterpreted: the resume never returns, and
         * the receiver's own `: any` return widens the int-typed call. */
        if (res_kind != TY_INT && res_kind != TY_ANY)
            out = call_wrap_reinterpret(e, out, res_kind, call->span);
        return out;
    }

    if (fn_type.kind == TY_FN &&
        e->unsafe_depth == 0 &&
        effect_row_contains_symbol(fn_type.as.fn.effect_row, e->sym_effect_unsafe)) {
        diag_emit(DIAG_ERROR, call->span,
                  "unsafe function '%s' requires an enclosing (unsafe ...)",
                  fn_binding->name->name);
        return NULL;
    }

    uint32_t expected_arity = 0;
    if (fn_type.kind == TY_FN) {
        expected_arity = fn_type.as.fn.arity;

        /* For closure bindings, the thunk function has an extra env parameter */
        if (fn_binding->closure_fn_binding) {
            expected_arity--;  /* Subtract the hidden env parameter */
        }
    } else if (fn_type.kind == TY_CONT) {
        /* Continuations are callable with exactly 1 argument (the resume value) */
        expected_arity = 1;
    }

    /* AR7: For variadic functions the rest param counts as one fixed slot;
     * partial application is allowed up to n_required (= arity - 1) args,
     * and any call with n_args >= n_required dispatches variadically. */
    bool fn_is_variadic = (fn_type.kind == TY_FN && fn_type.as.fn.is_variadic);
    uint32_t n_required = (fn_is_variadic && expected_arity > 0)
                         ? (expected_arity - 1)
                         : expected_arity;

    if (n_args < n_required && fn_type.kind == TY_FN) {
        /* r7rs-too-few-arguments-returns-a-procedure: Scheme has no partial
         * application.  In a Scheme program or library, a call with fewer
         * arguments than the procedure requires is an error when it runs
         * (R7RS 4.1.3): evaluate the arguments, as the call would have, then
         * raise an error object a program can `guard`.  The Turmeric-shaped
         * prelude keeps currying. */
        Expr *arity_err = scheme_arity_error(e, call, fn_binding, n_args, n_required,
                                             fn_is_variadic);
        if (arity_err) return arity_err;
        /* CY1: Partial application */
        Expr **pap_elab_args = (n_args > 0)
            ? (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *))
            : NULL;
        for (uint32_t i = 0; i < n_args; i++) {
            pap_elab_args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (!pap_elab_args[i]) return NULL;
        }
        return elab_partial_apply(e, call, fn_binding, fn_type, pap_elab_args, n_args);
    }
    /* AR8: Variadic dispatch -- build a cons list for surplus args */
    if (fn_is_variadic && (uint32_t)n_args >= (uint32_t)n_required) {
        uint32_t n_rest = n_args - n_required;
        uint32_t n_call_args = n_required + 1;  /* fixed args + the cons-list arg */
        Expr **call_args = (Expr **)arena_alloc(e->arena,
                            (n_call_args ? n_call_args : 1) * sizeof(Expr *));
        /* Elaborate fixed args */
        for (uint32_t i = 0; i < n_required; i++) {
            call_args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (!call_args[i]) return NULL;
            /* stdlib-int-stand-in-audit: a variadic callee's fixed
             * FUNCTION-typed parameter got none of the fixed-arity path's
             * shape check (plain-fn-typed-params-are-kind-matched / LT2), so
             * `compose-middleware-of`'s `^fat base : (fn [ptr<void>] nil)`
             * took a two-argument lambda with exit 0.  Same structural test,
             * same diagnostic. */
            if (call_args[i]->type.kind == TY_FN && fn_type.kind == TY_FN &&
                fn_type.as.fn.arg_full_types && i < fn_type.as.fn.arity) {
                const Type *declared_fn = fn_type.as.fn.arg_full_types[i];
                if (declared_fn && declared_fn->kind == TY_FN &&
                    !fn_type_structurally_compatible(call_args[i]->type, *declared_fn)) {
                    Buf sx; buf_init(&sx);
                    type_print(&sx, *declared_fn); buf_putc(&sx, '\0');
                    Buf sa; buf_init(&sa);
                    type_print(&sa, call_args[i]->type); buf_putc(&sa, '\0');
                    diag_emit_with_code(DIAG_ERROR, call_args[i]->span,
                                        TUR_E0001_TYPE_MISMATCH,
                                        "function '%s' arg %u: expected a function of type %s, "
                                        "got %s -- arity, argument types and result type must match",
                                        fn_binding && fn_binding->name ? fn_binding->name->name : "?",
                                        i + 1, sx.data, sa.data);
                    buf_free(&sx); buf_free(&sa);
                    return NULL;
                }
            }
            /* saffron-dynamic-surface-pass (`& rest : any`): a variadic
             * callee's FIXED parameters get none of the fixed-arity path's
             * coercions, so an unannotated (`any`) parameter of a Saffron
             * variadic defn received a bare int and cc rejected the call
             * (`incompatible type for argument 1`).  Widen exactly as the
             * fixed-arity `A <: any` rule does. */
            if (i < fn_type.as.fn.arity && fn_type.as.fn.arg_kinds &&
                fn_type.as.fn.arg_kinds[i] == TY_ANY &&
                call_args[i]->type.kind != TY_ANY)
                call_args[i] = elab_coerce_to_any(e, call_args[i]);
            /* r7rs-lang-plan R7: and the reverse -- an `any` argument into a
             * CONCRETE fixed parameter takes the seam's checked cast, as the
             * fixed-arity path does (D5).  Without it `(string->list s)` with
             * `s : any` handed a tagged box to a `const char *` parameter and
             * cc rejected the call. */
            else if (i < fn_type.as.fn.arity && fn_type.as.fn.arg_kinds &&
                     call_args[i]->type.kind == TY_ANY) {
                TypeKind pk = (TypeKind)fn_type.as.fn.arg_kinds[i];
                if (pk != TY_ANY && pk != TY_UNKNOWN && pk != TY_TYVAR && pk != TY_NIL) {
                    Type want = (fn_type.as.fn.arg_full_types && fn_type.as.fn.arg_full_types[i])
                                    ? *fn_type.as.fn.arg_full_types[i]
                                    : type_from_kind(pk);
                    Expr *un = elab_any_unbox_to(e, call_args[i], want, call_args[i]->span);
                    elab_any_cast_note_callee(e, un, fn_binding);
                    if (un) call_args[i] = un;
                }
            }
            /* r7rs-dead-mistyped-call-refused-at-compile-time: a CONCRETE
             * argument of the wrong type into a concrete fixed parameter.
             * Nothing here checked it, so in Scheme `(vector-fill! 5 0)` or
             * `(vector->list 'a)` compiled with a C pointer warning and then
             * crashed or answered garbage.  In Scheme source, widen it and take
             * the checked cast above, as the fixed-arity path now does, so the
             * call raises when it runs.  An exact integer into a float
             * parameter keeps the C conversion it always had; a float into an
             * integer one does not (`(make-vector 2.5)` sized the vector from
             * the float's bits under --interpret). */
            else if (i < fn_type.as.fn.arity && fn_type.as.fn.arg_kinds &&
                     lang_span_is_scheme(call->span)) {
                TypeKind pk = (TypeKind)fn_type.as.fn.arg_kinds[i];
                TypeKind ak = call_args[i]->type.kind;
                const Type *pf = fn_type.as.fn.arg_full_types
                                     ? fn_type.as.fn.arg_full_types[i] : NULL;
                bool a_float = ak == TY_FLOAT || ak == TY_FLOAT32 || ak == TY_FLOAT64;
                bool p_float = pk == TY_FLOAT || pk == TY_FLOAT32 || pk == TY_FLOAT64;
                bool numeric_ok = typekind_is_numeric(ak) && typekind_is_numeric(pk) &&
                                  (p_float || !a_float);
                bool mismatch =
                    (ak != pk && !numeric_ok) ||
                    (ak == pk && pf &&
                     (pf->kind == TY_STRUCT || pf->kind == TY_ADT || pf->kind == TY_APP) &&
                     !type_eq(call_args[i]->type, *pf));
                if (mismatch && pk != TY_ANY && pk != TY_UNKNOWN && pk != TY_TYVAR &&
                    pk != TY_NIL && ak != TY_UNKNOWN && ak != TY_TYVAR) {
                    Expr *w = elab_coerce_to_any(e, call_args[i]);
                    Expr *un = w ? elab_any_unbox_to(e, w, pf ? *pf : type_from_kind(pk),
                                                     call_args[i]->span)
                                 : NULL;
                    elab_any_cast_note_callee(e, un, fn_binding);
                    if (un) call_args[i] = un;
                }
            }
        }
        /* Build cons-list expression for rest args */
        Expr *rest_expr = NULL;
        /* r7rs-lang-plan R7: `(f a ... (__rest-chain c))` -- `c` IS the rest
         * list, already a `(Cons any)` chain; pass it through rather than
         * packing it as one element.  Only the H8 variadic adaptor writes
         * this, and only for an `any` rest. */
        if (n_rest == 1) {
            const Form *lf = call->as.list.items[1 + n_required];
            if (lf->tag == F_LIST && lf->as.list.len == 2 && lf->as.list.items[0]->tag == F_SYM &&
                strcmp(lf->as.list.items[0]->as.sym->name, "__rest-chain") == 0 &&
                fn_type.as.fn.rest_kind == TY_ANY) {
                Expr *cv = elab_form(e, lf->as.list.items[1]);
                if (!cv) return NULL;
                rest_expr = cv;
            }
        }
        /* Homogeneity / result specialization (hoisted so they survive past the
         * cons-list build): a polymorphic-tyvar rest (`[& xs :A]`) names a
         * single type variable A.  Bind A to the first rest arg, then -- once
         * the cons list is built -- substitute A into the declared result type
         * so `(list-of 7)`'s `(List A)` return becomes `(List int)`. */
        Type tyvar_first;
        bool tyvar_first_set = false;
        const char *rest_tyvar_name =
            (fn_type.as.fn.rest_full_type &&
             fn_type.as.fn.rest_full_type->kind == TY_TYVAR)
                ? fn_type.as.fn.rest_full_type->as.tyvar_.name : NULL;
        /* saffron-dynamic-surface-pass (`& rest : any`): the rest list of a
         * `: any` rest is the stdlib `(Cons any)` monomorph (see the rest
         * binding in elab_fns.c), not the int64 `__tur_cons_of` cell -- an
         * `any` is a two-word tagged box and the cell's head slot is one
         * word; before this the check below rejected `(f 1 2 3)` outright
         * and an already-`any` argument compiled to `aggregate value used
         * where an integer was expected`.  Build the chain as the source
         * would: `(make-struct Cons (:: e0 any) (make-struct Cons (:: e1
         * any) ... (:: 0 (Cons any))))`, and elaborate THAT, so every
         * element takes the ordinary widen and both back ends build the
         * same cell they build for `(list 1 "two" 7.1)`. */
        bool rest_any = fn_type.as.fn.rest_kind == TY_ANY &&
                        !(fn_type.as.fn.rest_full_type &&
                          fn_type.as.fn.rest_full_type->kind != TY_ANY);
        if (rest_expr) {
            /* the __rest-chain passthrough above */
        } else if (rest_any) {
            Arena *fa = e->arena;
            Span sp = call->span;
            const Symbol *cons_sym = intern_cstr(e->st, "Cons");
            const Symbol *any_sym  = intern_cstr(e->st, "any");
            Form **ty_items = (Form **)arena_alloc(fa, 2 * sizeof(Form *));
            ty_items[0] = form_sym(fa, sp, cons_sym);
            ty_items[1] = form_sym(fa, sp, any_sym);
            Form *cons_any_ty = form_list(fa, sp, ty_items, 2);
            Form **nil_items = (Form **)arena_alloc(fa, 3 * sizeof(Form *));
            nil_items[0] = form_sym(fa, sp, e->sym_ascribe);
            nil_items[1] = form_int(fa, sp, 0);
            nil_items[2] = cons_any_ty;
            Form *chain = form_list(fa, sp, nil_items, 3);
            for (int32_t ri = (int32_t)n_rest - 1; ri >= 0; ri--) {
                Form *item = call->as.list.items[1 + n_required + ri];
                Form **asc = (Form **)arena_alloc(fa, 3 * sizeof(Form *));
                asc[0] = form_sym(fa, item->span, e->sym_ascribe);
                asc[1] = item;
                asc[2] = form_sym(fa, item->span, any_sym);
                Form **ms = (Form **)arena_alloc(fa, 4 * sizeof(Form *));
                ms[0] = form_sym(fa, item->span, e->sym_make_struct);
                ms[1] = form_sym(fa, item->span, cons_sym);
                ms[2] = form_list(fa, item->span, asc, 3);
                ms[3] = chain;
                chain = form_list(fa, item->span, ms, 4);
            }
            rest_expr = elab_form(e, chain);
            if (!rest_expr) return NULL;
        } else if (n_rest == 0) {
            rest_expr = expr_new(e->arena, EX_INT_LIT, TYPE_INT, call->span);
            rest_expr->as.i = 0;  /* nil = 0 */
        } else {
            Expr **rest_items = (Expr **)arena_alloc(e->arena, n_rest * sizeof(Expr *));
            TypeKind rk = fn_type.as.fn.rest_kind;
            /* Typed-variadic: when the rest element is a user-defined type the
             * declaration carries its full Type so we can compare identity
             * (struct/ADT def pointer, applied type args) rather than only the
             * coarse TypeKind.  Primitive rest keeps rest_full_type == NULL and
             * uses the fast TypeKind path below.  A polymorphic-tyvar rest also
             * carries a (TY_TYVAR) rest_full_type purely to name A for the
             * result specialization above; it is NOT a type-identity match, so
             * route it through the homogeneity path (rk == TY_TYVAR). */
            Type *rest_full = fn_type.as.fn.rest_full_type;
            if (rest_full && rest_full->kind == TY_TYVAR) rest_full = NULL;
            bool rest_err = false;
            for (uint32_t i = 0; i < n_rest; i++) {
                rest_items[i] = elab_form(e, call->as.list.items[1 + n_required + i]);
                if (!rest_items[i]) return NULL;
                /* AR10: type-check each rest arg against the declared rest element type */
                TypeKind ak = rest_items[i]->type.kind;
                bool rest_ok;
                if (rest_full) {
                    /* Full-type comparison for user-defined rest (opaque /
                     * struct / ADT / type application). */
                    rest_ok = type_eq(rest_items[i]->type, *rest_full);
                } else {
                    rest_ok = (ak == rk);
                    /* Polymorphic rest element (TY_TYVAR): A is one type
                     * variable, so all rest args must unify to a single type.
                     * Bind A to the first arg; compare the rest by identity. */
                    if (!rest_ok && rk == TY_TYVAR) {
                        if (!tyvar_first_set) {
                            tyvar_first = rest_items[i]->type;
                            tyvar_first_set = true;
                            rest_ok = true;
                        } else {
                            rest_ok = type_eq(rest_items[i]->type, tyvar_first);
                        }
                    }
                }
                if (!rest_ok) {
                    const char *fn_name = (fn_binding && fn_binding->name) ? fn_binding->name->name : "?";
                    const char *expected = rest_full
                        ? type_name(*rest_full)
                        : (rk == TY_TYVAR && tyvar_first_set
                            ? type_name(tyvar_first)
                            : typekind_to_string(rk));
                    const char *got = type_name(rest_items[i]->type);
                    diag_emit(DIAG_ERROR,
                              call->as.list.items[1 + n_required + i]->span,
                              "variadic call to '%s': rest arg %u has wrong type "
                              "(expected %s, got %s)",
                              fn_name, i, expected, got);
                    /* Keep checking the remaining rest args so every mismatch
                     * is reported in one pass, then fail. */
                    rest_err = true;
                }
            }
            if (rest_err) return NULL;
            rest_expr = expr_new(e->arena, EX_CONS_LIST, TYPE_INT, call->span);
            rest_expr->as.cons_list_.items = rest_items;
            rest_expr->as.cons_list_.n = n_rest;
            rest_expr->as.cons_list_.item_kind = fn_type.as.fn.rest_kind;
        }
        call_args[n_required] = rest_expr;
        /* Determine result type */
        Type result_type = fn_type.as.fn.result_full_type
                           ? *fn_type.as.fn.result_full_type
                           : type_from_kind(fn_type.as.fn.result_kind);
        /* Specialize a polymorphic-tyvar rest into the result type: when the
         * rest element is `:A` and the call's args pin A to a concrete type,
         * substitute A throughout the declared result (e.g. `(List A)` becomes
         * `(List int)` for `(list-of 7)`), so downstream consumers recover the
         * element type instead of a bare tyvar.  With zero rest args A is
         * unconstrained and stays as declared. */
        if (rest_tyvar_name && tyvar_first_set) {
            CallTypeBinding rest_bind;
            rest_bind.name = rest_tyvar_name;
            rest_bind.type = tyvar_first;
            result_type = call_instantiate_type(e, &result_type, &rest_bind, 1);
        }
        Expr *out = expr_new(e->arena, EX_CALL, result_type, call->span);
        out->as.call_.fn_binding = fn_binding;
        out->as.call_.args = call_args;
        out->as.call_.n_args = n_call_args;
        out->as.call_.fn_expr = NULL;
        out->as.call_.dict_arg = NULL;
        out->as.call_.is_poly_call = false;
        out->as.call_.poly_arg_mask = 0;
        out->as.call_.abi_bindings = NULL;
        out->as.call_.n_abi_bindings = 0;
        return out;
    }
    if (n_args > expected_arity && fn_type.kind == TY_FN) {
        /* r7rs-dead-mistyped-call-refused-at-compile-time: Scheme has no
         * over-application either -- `(f 1 2)` of a one-argument `f` is an
         * error when it runs (R7RS 4.1.3), not a compile-time refusal. */
        Expr *arity_err = scheme_arity_error(e, call, fn_binding, n_args,
                                             expected_arity, false);
        if (arity_err) return arity_err;
        /* CY2: Over-application */
        TypeKind result_kind = fn_type.as.fn.result_kind;
        if (result_kind != TY_PTR_VOID) {
            diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0002_ARITY_MISMATCH,
                                "function '%s' returns %s, which is not callable -- "
                                "did you mean to pass all %u argument(s)?",
                                fn_binding->name->name,
                                type_name(type_from_kind(result_kind)),
                                expected_arity);
            return NULL;
        }
        /* Elaborate the first expected_arity args (used in inner call) */
        Expr **inner_args = (Expr **)arena_alloc(e->arena, expected_arity * sizeof(Expr *));
        for (uint32_t i = 0; i < expected_arity; i++) {
            inner_args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (!inner_args[i]) return NULL;
        }
        /* Build inner call expression */
        Type inner_result_type = type_from_kind(result_kind);
        Expr *inner_call = expr_new(e->arena, EX_CALL, inner_result_type, call->span);
        inner_call->as.call_.fn_binding = fn_binding;
        inner_call->as.call_.args = inner_args;
        inner_call->as.call_.n_args = expected_arity;
        inner_call->as.call_.fn_expr = NULL;
        inner_call->as.call_.dict_arg = NULL;
        inner_call->as.call_.is_poly_call = false;
        inner_call->as.call_.poly_arg_mask = 0;
        /* Create a let-binding for the intermediate closure result */
        char oar_name[32];
        snprintf(oar_name, sizeof(oar_name), "__oar%u", elab_fresh_id(e));
        const Symbol *oar_sym = symtab_intern(e->st, strslice(oar_name, (uint32_t)strlen(oar_name)));
        Binding *oar_binding = binding_new(e, oar_sym, inner_result_type, false, false, call->span);
        /* Set closure_fn_binding if the inner result is a closure */
        /* (We don't know at this point, but EX_CLOSURE wrapping in emit handles it dynamically) */
        /* Elaborate remaining args */
        uint32_t n_outer = n_args - expected_arity;
        Expr **outer_args = (Expr **)arena_alloc(e->arena, n_outer * sizeof(Expr *));
        for (uint32_t i = 0; i < n_outer; i++) {
            outer_args[i] = elab_form(e, call->as.list.items[1 + expected_arity + i]);
            if (!outer_args[i]) return NULL;
        }
        /* Result type of outer call: TY_INT as default for opaque fat closures */
        Type outer_result_type = TYPE_INT;
        if (fn_type.as.fn.result_full_type &&
            fn_type.as.fn.result_full_type->kind == TY_FN) {
            outer_result_type = type_from_kind(fn_type.as.fn.result_full_type->as.fn.result_kind);
        }
        Expr *outer_call = expr_new(e->arena, EX_CALL, outer_result_type, call->span);
        outer_call->as.call_.fn_binding = oar_binding;
        outer_call->as.call_.args = outer_args;
        outer_call->as.call_.n_args = n_outer;
        outer_call->as.call_.fn_expr = NULL;
        outer_call->as.call_.dict_arg = NULL;
        outer_call->as.call_.is_poly_call = false;
        outer_call->as.call_.poly_arg_mask = 0;
        /* Wrap in EX_LET */
        LetBinding *oar_let_bs = (LetBinding *)arena_alloc(e->arena, sizeof(LetBinding));
        oar_let_bs->binding = oar_binding;
        oar_let_bs->init = inner_call;
        Expr *oar_let = expr_new(e->arena, EX_LET, outer_result_type, call->span);
        oar_let->as.let_.bindings = oar_let_bs;
        oar_let->as.let_.n = 1;
        oar_let->as.let_.body = outer_call;
        return oar_let;
    }
    if (n_args != expected_arity) {
        /* Phase 8: Enhanced arity mismatch diagnostic with error code */
        diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0002_ARITY_MISMATCH,
                            "function '%s' expects %u argument(s), got %u",
                            fn_binding->name->name, expected_arity, n_args);
        return NULL;
    }

    /* Elaborate arguments */
    Expr **args = (Expr **)arena_alloc(e->arena, n_args * sizeof(Expr *));
    /* poly-hof-constrained-arg-baked-carrier: the eta-expansion look-ahead may
     * elaborate a *sibling* concrete arg early (to pin a tyvar appearing in a
     * function-typed parameter); `arg_done[i]` records that so the main loop
     * does not re-elaborate it.  Zero-initialize both arrays. */
    for (uint32_t ai = 0; ai < n_args; ai++) args[ai] = NULL;
    bool *arg_done = (bool *)arena_alloc(e->arena, (n_args ? n_args : 1) * sizeof(bool));
    for (uint32_t ai = 0; ai < n_args; ai++) arg_done[ai] = false;
    CallTypeBinding type_bindings[16];
    uint8_t n_type_bindings = 0;
    for (uint8_t bi = 0; bi < 16; bi++) type_bindings[bi].name = NULL;
    /* open-arg-first-binds-call-result-as-int: arguments to ground after the
     * loop (allocated on first use). */
    bool *open_first = NULL;
    bool *open_first_strict = NULL;   /* accepted before it could be checked */
    /* generic-return-type-not-inferred-from-context: capture the enclosing
     * expected-type channel (pushed by (:: e T), typed-let, or the defn
     * return slot) before clearing it for sub-arg elaboration -- so the
     * result-type collection below sees the outer context exactly once. */
    Type *saved_expected_return = e->expected_type;
    e->expected_type = NULL;
    for (uint32_t i = 0; i < n_args; i++) {
        /* Bidirectional inference (constrained-generic-as-value-bakes-
         * representative.md): when the arg is a lambda form and the parameter
         * expects a concrete function type, push that type onto the expected-type
         * channel so the lambda's un-annotated params can be typed from context
         * (see elab_fn).  Scoped to lambda args so no other arg elaboration is
         * affected; restored immediately after. */
        const Form *arg_form = call->as.list.items[1 + i];
        /* Expected concrete fn type for this parameter (if any). */
        const Type *exp_param_fn = NULL;
        /* The same slot, when it declares a parametric ADT application -- the
         * channel a bare constructor argument needs.  See
         * arg_form_is_ctor_call_of. */
        const Type *exp_param_app = NULL;
        if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
            uint32_t idx = fn_binding->closure_fn_binding ? i + 1 : i;
            if (idx < fn_type.as.fn.arity && fn_type.as.fn.arg_full_types[idx]) {
                if (fn_type.as.fn.arg_full_types[idx]->kind == TY_FN)
                    exp_param_fn = fn_type.as.fn.arg_full_types[idx];
                else if (fn_type.as.fn.arg_full_types[idx]->kind == TY_APP)
                    exp_param_app = fn_type.as.fn.arg_full_types[idx];
            }
        }
        /* poly-hof-constrained-arg-baked-carrier: when the function-typed
         * parameter is itself polymorphic (`f : (fn [A] int)` on a HOF
         * quantified over A) the param type is still abstract here, so the
         * eta-expansion below would not fire and the bare constrained-generic
         * arg (`count-it`) would be coerced to its int64-carrier representative
         * instance -- baking the wrong typeclass dispatch into the per-call-site
         * wrapper.  Resolve the param's tyvars from *sibling* arguments that are
         * exactly a bare tyvar (`a : A`): elaborate just those siblings early
         * (cached via arg_done) to learn the concrete type, then instantiate the
         * param fn type so the eta look-ahead sees a concrete `(fn [Box] int)`
         * and specializes `count-it` per instantiation.  Gated to a bare global
         * fn symbol arg whose param fn type carries named tyvars, so ordinary
         * calls are untouched. */
        const Type *eta_param_fn = exp_param_fn;
        Type eta_param_fn_inst;
        if (exp_param_fn && call_type_has_named_tyvar(exp_param_fn) &&
            arg_form->tag == F_SYM && fn_type.kind == TY_FN &&
            fn_type.as.fn.arg_full_types) {
            CallTypeBinding lab[16];
            uint8_t n_lab = 0;
            for (uint8_t bi = 0; bi < 16; bi++) lab[bi].name = NULL;
            for (uint32_t j = 0; j < n_args; j++) {
                if (j == i) continue;
                uint32_t pj = fn_binding->closure_fn_binding ? j + 1 : j;
                if (pj >= fn_type.as.fn.arity) continue;
                const Type *pjt = fn_type.as.fn.arg_full_types[pj];
                if (!pjt || pjt->kind != TY_TYVAR || !pjt->as.tyvar_.name) continue;
                if (!type_mentions_tyvar_name(exp_param_fn, pjt->as.tyvar_.name)) continue;
                uint8_t exist = 0;
                if (call_find_type_binding(lab, n_lab, pjt->as.tyvar_.name, &exist)) continue;
                /* Elaborate the sibling arg once and cache it for the main loop. */
                if (!arg_done[j]) {
                    args[j] = elab_form(e, call->as.list.items[1 + j]);
                    arg_done[j] = true;
                }
                if (args[j] && args[j]->type.kind != TY_TYVAR && n_lab < 16) {
                    lab[n_lab].name = pjt->as.tyvar_.name;
                    lab[n_lab].type = args[j]->type;
                    n_lab++;
                }
            }
            if (n_lab > 0) {
                /* Instantiate the function-typed parameter's tyvars locally
                 * (call_instantiate_type has no TY_FN case and would return it
                 * unchanged).  Only the full-type arrays the eta look-ahead
                 * reads need substituting. */
                eta_param_fn_inst = *exp_param_fn;
                uint32_t ar = exp_param_fn->as.fn.arity;
                if (exp_param_fn->as.fn.arg_full_types) {
                    Type **afts = (Type **)arena_alloc(e->arena,
                        (ar ? ar : 1) * sizeof(Type *));
                    for (uint8_t k = 0; k < ar; k++) {
                        if (exp_param_fn->as.fn.arg_full_types[k]) {
                            afts[k] = (Type *)arena_alloc(e->arena, sizeof(Type));
                            *afts[k] = call_instantiate_type(e,
                                exp_param_fn->as.fn.arg_full_types[k], lab, n_lab);
                        } else {
                            afts[k] = NULL;
                        }
                    }
                    eta_param_fn_inst.as.fn.arg_full_types = afts;
                }
                if (exp_param_fn->as.fn.result_full_type) {
                    Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *rft = call_instantiate_type(e,
                        exp_param_fn->as.fn.result_full_type, lab, n_lab);
                    eta_param_fn_inst.as.fn.result_full_type = rft;
                }
                if (!call_type_has_named_tyvar(&eta_param_fn_inst))
                    eta_param_fn = &eta_param_fn_inst;
            }
        }
        /* Eta-expand a bare constrained-generic fn arg so its body dispatches
         * the real instance (see try_eta_expand_generic_fn_arg). */
        Expr *eta = (eta_param_fn && !arg_done[i])
            ? try_eta_expand_generic_fn_arg(e, arg_form, eta_param_fn) : NULL;
        if (eta) {
            args[i] = eta;
        } else if (arg_done[i]) {
            /* Already elaborated by a prior sibling look-ahead. */
        } else {
            /* Bidirectional inference: push the param's concrete fn type while
             * elaborating a lambda arg so its un-annotated params type from
             * context.  Scoped to lambda args; restored immediately after. */
            bool pushed_expected = false;
            if (exp_param_fn && arg_form->tag == F_LIST && arg_form->as.list.len >= 1 &&
                arg_form->as.list.items[0]->tag == F_SYM &&
                (arg_form->as.list.items[0]->as.sym == e->sym_fn ||
                 arg_form->as.list.items[0]->as.sym == e->sym_lambda)) {
                e->expected_type = (Type *)exp_param_fn;
                pushed_expected = true;
            } else if (arg_form_is_ctor_call_of(e, arg_form, exp_param_app)) {
                /* Bare constructor argument: hand it the parameter's own
                 * family so it selects that monomorph rather than the carrier
                 * base.  Same channel the lambda case above uses, same
                 * immediate restore. */
                e->expected_type = (Type *)exp_param_app;
                pushed_expected = true;
            }
            args[i] = elab_form(e, call->as.list.items[1 + i]);
            if (pushed_expected) e->expected_type = NULL;
        }
        arg_done[i] = true;
        if (!args[i]) return NULL;
        TypeKind expected_arg_kind = TY_INT;
        if (fn_type.kind == TY_FN) {
            uint32_t fn_arg_idx = i;
            if (fn_binding->closure_fn_binding) {
                /* Closure thunk arg[0] is hidden env ptr. */
                fn_arg_idx = i + 1;
            }
            expected_arg_kind = fn_type.as.fn.arg_kinds[fn_arg_idx];
        }

        /* Phase HRT1: Detect rank-2 poly param and wrap arg in EX_POLY_WRAP.
         * arg_full_types[fn_arg_idx] is TY_FORALL → this is a rank-2 param. */
        bool is_rank2_param = false;
        const Type *rank2_forall_ty = NULL;  /* Slice 3: the param's TY_FORALL */
        if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
            uint32_t fn_arg_idx2 = i;
            if (fn_binding->closure_fn_binding) fn_arg_idx2 = i + 1;
            if (fn_arg_idx2 < fn_type.as.fn.arity) {
                Type *aft = fn_type.as.fn.arg_full_types[fn_arg_idx2];
                /* F1-1: TY_EXISTS-typed params are constrained existential
                 * values, not rank-2 functions; pass the arg through as-is
                 * and let the regular kind check accept the matching
                 * TY_EXISTS argument from `pack`. */
                if (aft && aft->kind == TY_FORALL) {
                    is_rank2_param = true;
                    rank2_forall_ty = aft;
                }
            }
        }

        /* gadt-length-index-not-enforced: a bare GADT argument whose index is
         * fully known (gadt_form / an annotated binding's form) meets a
         * parameter declared at an application of the same GADT.  Give the
         * argument that type, as the ascription `(:: arg (Vec (Succ Zero)))`
         * would, so the parameter's index variables bind from it and a
         * mismatch is the ordinary type error: `(lhead (ltail v))` binds
         * `ltail`'s `n` from `v`'s index instead of leaving `(Vec n)` open.
         * Only there -- a bare parameter keeps the bare argument. */
        if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
            args[i]->type.kind == TY_ADT && args[i]->type.as.adt_.def &&
            args[i]->type.as.adt_.def->is_gadt) {
            uint32_t gfi = fn_binding->closure_fn_binding ? i + 1 : i;
            const Type *gft = (gfi < fn_type.as.fn.arity)
                ? fn_type.as.fn.arg_full_types[gfi] : NULL;
            if (gft && gft->kind == TY_APP &&
                type_adt_app_def(gft) == args[i]->type.as.adt_.def)
                args[i] = gadt_refine_to_index(e, args[i]);
        }
        bool arg_ok = (args[i]->type.kind == expected_arg_kind);
        /* typed-c-abi-function-pointers: a `(c-fn [A...] R)` parameter is a
         * bare C function pointer.  The same-kind match above would admit ANY
         * TY_FN argument -- including a capturing closure or a wrong-shape fn --
         * so enforce the real contract here: the argument must be a captureless
         * (non-boxed) fn of the exact signature.  Capturing closures (which
         * carry an environment that a raw C pointer cannot represent) and
         * arity/signature mismatches are hard errors. */
        if (expected_arg_kind == TY_FN && fn_type.kind == TY_FN &&
                fn_type.as.fn.arg_full_types) {
            uint32_t cfn_idx = fn_binding->closure_fn_binding ? i + 1 : i;
            Type *exp_cfn = (cfn_idx < fn_type.as.fn.arity)
                ? fn_type.as.fn.arg_full_types[cfn_idx] : NULL;
            if (exp_cfn && exp_cfn->kind == TY_FN && exp_cfn->as.fn.cfnptr) {
                if (args[i]->type.kind != TY_FN) {
                    /* Non-fn argument: let the generic mismatch path report it. */
                    arg_ok = false;
                } else if (args[i]->type.as.fn.boxed) {
                    Buf cap; buf_init(&cap);
                    if (args[i]->kind == EX_CLOSURE && args[i]->as.closure_.closure) {
                        struct Closure *clo = args[i]->as.closure_.closure;
                        for (uint8_t ci = 0; ci < clo->n_captures; ci++) {
                            if (ci > 0) buf_puts(&cap, ", ");
                            if (clo->captures[ci] && clo->captures[ci]->name)
                                buf_puts(&cap, clo->captures[ci]->name->name);
                        }
                    }
                    buf_putc(&cap, '\0');
                    diag_emit(DIAG_ERROR, args[i]->span,
                        "TUR-E0295: argument %u of '%s' is a capturing closure, "
                        "but the parameter is a C function pointer (c-fn) and "
                        "cannot carry an environment%s%s. Captured values would "
                        "be silently dropped at the c-fn boundary. Pass a "
                        "captureless function (no free variables), change the "
                        "parameter type from (c-fn ...) to (fn ...), or hoist "
                        "the captures out.",
                        i + 1, fn_binding->name->name,
                        cap.data[0] ? "; captures: " : "", cap.data);
                    buf_free(&cap);
                    return NULL;
                } else if (args[i]->type.as.fn.arity != exp_cfn->as.fn.arity) {
                    diag_emit(DIAG_ERROR, args[i]->span,
                        "argument %u of '%s' has the wrong arity for C function "
                        "pointer parameter: expected %u parameter(s), got %u",
                        i + 1, fn_binding->name->name,
                        (unsigned)exp_cfn->as.fn.arity,
                        (unsigned)args[i]->type.as.fn.arity);
                    return NULL;
                } else {
                    /* tyvars-in-c-fn: a (c-fn [...] R) parameter on a generic
                     * defn may carry type-variable arg/result positions (e.g.
                     * `keyeq : (c-fn [K K] bool)` on `map-assoc-eq [K V]`). Those
                     * positions lower their kind slot to the int64 carrier
                     * (TY_INT) at parse time, so a per-kind compare here would
                     * spuriously reject a captureless comparator whose concrete
                     * arg kind differs from the carrier (e.g. a `(fn [cstr cstr]
                     * bool)` for a `(Map cstr int)`). Treat a tyvar position in
                     * the *expected* c-fn as a wildcard -- it matches any single
                     * concrete kind. The boxed (capturing-closure) and arity
                     * checks above are unaffected, so the contract that matters
                     * (no environment, right arity) is still enforced. */
                    bool exp_res_tyvar = exp_cfn->as.fn.result_full_type &&
                        exp_cfn->as.fn.result_full_type->kind == TY_TYVAR;
                    bool sig_ok = exp_res_tyvar ||
                        (args[i]->type.as.fn.result_kind
                                   == exp_cfn->as.fn.result_kind);
                    for (uint8_t si = 0; sig_ok && si < exp_cfn->as.fn.arity; si++) {
                        bool exp_arg_tyvar = exp_cfn->as.fn.arg_full_types &&
                            exp_cfn->as.fn.arg_full_types[si] &&
                            exp_cfn->as.fn.arg_full_types[si]->kind == TY_TYVAR;
                        if (!exp_arg_tyvar &&
                            args[i]->type.as.fn.arg_kinds[si]
                                != exp_cfn->as.fn.arg_kinds[si])
                            sig_ok = false;
                    }
                    if (!sig_ok) {
                        Buf eb; buf_init(&eb); type_print(&eb, *exp_cfn); buf_putc(&eb, '\0');
                        Buf gb; buf_init(&gb); type_print(&gb, args[i]->type); buf_putc(&gb, '\0');
                        diag_emit(DIAG_ERROR, args[i]->span,
                            "argument %u of '%s' has type %s, which does not "
                            "match the C function pointer parameter type %s",
                            i + 1, fn_binding->name->name, gb.data, eb.data);
                        buf_free(&eb); buf_free(&gb);
                        return NULL;
                    }
                    arg_ok = true;
                }
            }
        }
        /* Nominal identity: a same-kind struct/opaque/ADT argument must be the
         * *same* type, not merely the same TypeKind.  Full param types come from
         * arg_full_types (Phase 1).  Placed before the escape hatches: those only
         * ever set arg_ok from false->true for cross-kind coercions, so demoting a
         * spurious same-kind match here cannot resurrect a real coercion.
         * See docs/archive/history/positional-nominal-type-identity-fix-plan.md.
         *
         * session-type-eq-ignores-the-protocol: TY_SESSION joins the strict set
         * for the same reason. A session endpoint's TypeKind says only "some
         * channel"; the protocol it is partway through lives in the full type.
         * Comparing kinds alone let an endpoint be passed to a function
         * declaring a *different* protocol -- including handing a client the
         * server's end of one `make-session` -- with no diagnostic, which is
         * precisely the mismatch session types exist to catch. `type_eq` now
         * compares protocols equirecursively, so folded `Rec[X, P]` and its
         * unfolding still agree here. */
        if (arg_ok && (expected_arg_kind == TY_STRUCT || expected_arg_kind == TY_ADT ||
                       expected_arg_kind == TY_SESSION || expected_arg_kind == TY_ROLE) &&
                fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
            uint32_t nidx = fn_binding->closure_fn_binding ? i + 1 : i;
            if (nidx < fn_type.as.fn.arity) {
                Type *ef = fn_type.as.fn.arg_full_types[nidx];
                if (ef && (ef->kind == TY_STRUCT || ef->kind == TY_ADT ||
                           ef->kind == TY_SESSION || ef->kind == TY_ROLE) &&
                        !type_eq(args[i]->type, *ef)) {
                    arg_ok = false;
                }
            }
        }
        /* Phase HRT/G2: A TY_TYVAR parameter (named type variable like :a) accepts any argument.
         * The concrete type is resolved per-arm inside a GADT match. */
        if (!arg_ok && expected_arg_kind == TY_TYVAR) {
            arg_ok = true;
            /* captureless-closure-lost-through-untyped-vec: a *captureless*
             * closure (e.g. the SF returned by a nullary `(invert)`) is
             * codegen'd as a BARE fn pointer, not a { thunk, env } fat box.
             * When it escapes into a polymorphic (TY_TYVAR) parameter -- the
             * `val :A` of `vec-push!`, the `a :A`/`b :A` of a generic helper --
             * it is stored as the opaque int64_t carrier (a raw code address).
             * A later `vec-get` + `(:: v :ptr<void>)` + `^fat` dispatch then
             * reads that code address as slot 0 of a fat box and fat-calls
             * garbage -> segfault, and the carrier has lost all fn-pointer
             * provenance by then so the retrieval side cannot recover.  Box the
             * captureless closure here, at the escape point, via EX_FN_TO_FAT
             * (the same { shim, fn } box the ^fat auto-shim produces) so every
             * closure value stored through a generic carrier is a uniform fat
             * box.  A *capturing* closure's value is TY_PTR_VOID (already a fat
             * box) and a boxed TY_FN is left untouched; only a bare, unboxed
             * TY_FN is shimmed.  Mirrors the ^fat auto-shim arity bound (<=5). */
            /* arrow-instance-closure-erased-to-words (`pipe` over ^fat arrows):
             * a `^fat` binding, or a fat-normalized parameter, already HOLDS a
             * { thunk, env } box despite its unboxed static type -- the same
             * already-fat test the ^fat sink branch makes, asked of the
             * representation decision (repr_of_binding).  Shimming it here
             * boxed the box: slot 0's `__tur_fatshim_double_double` then
             * called the inner box's address as code (SIGSEGV).  Mark the
             * use boxed instead, keeping its precise signature for M7. */
            if (args[i]->kind == EX_VAR && args[i]->as.var.binding &&
                args[i]->type.kind == TY_FN && !args[i]->type.as.fn.boxed &&
                (args[i]->as.var.binding->is_fat ||
                 args[i]->as.var.binding->is_param) &&
                repr_of_binding(args[i]->as.var.binding, REPR_POS_PARAM) ==
                    REPR_FAT_HANDLE) {
                args[i]->type.as.fn.boxed = true;
            }
            if (args[i]->type.kind == TY_FN && !args[i]->type.as.fn.boxed) {
                uint32_t inner_arity = args[i]->type.as.fn.arity;
                if (inner_arity >= 1 && inner_arity <= TUR_FAT_SHIM_MAX_ARITY) {
                    /* M7 fix direction 1: keep the precise fn signature on the
                     * boxed shim's static type so a function value escaping into
                     * a type-variable parameter -- e.g. `(some add1)` binding
                     * `some`'s `A` -- binds the tyvar to `(fn [int] int)` rather
                     * than the opaque `ptr<void>` carrier.  This lets the HKT
                     * by-value monomorphization recover the result element of the
                     * Applicative `ap` shape
                     * (docs/archive/history/m7-hkt-ap-fn-element-carrier-erasure.md).
                     * The runtime value is still a fat box (EX_FN_TO_FAT emits a
                     * `void *` regardless, reading inner->type); only the static
                     * type changes.  The clone is marked `boxed` so downstream
                     * auto-shim sites do not double-box. */
                    Type *bt = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *bt = args[i]->type;
                    bt->as.fn.boxed = true;
                    Type shim_ty = *bt;
                    Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, shim_ty,
                                          args[i]->span);
                    shim->as.fn_to_fat_.inner = args[i];
                    args[i] = shim;
                }
            }
        }
        /* poly-closure-result-specialization (Stage A2): the symmetric rule for
         * a tyvar-typed *argument*.  Inside a generic body, a parameter such as
         * `x : A` carries TY_TYVAR; passing it to another generic-typed callee
         * (e.g. `(fv x)` where `fv :(fn [A] B)`) lowers the callee's arg kind to
         * a non-tyvar carrier (TY_STRUCT), so the tyvar-parameter hatch above
         * does not fire.  Accept any tyvar argument: its concrete type is
         * resolved per monomorphization at the call site that instantiates the
         * enclosing generic defn. */
        if (!arg_ok && args[i]->type.kind == TY_TYVAR) {
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID && args[i]->type.kind == TY_FN) {
            /* Allow passing a function value where callback pointer is expected.
             * If this is a rank-2 param, we'll wrap it in a poly wrapper below. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID &&
                (args[i]->type.kind == TY_FORALL || args[i]->type.kind == TY_EXISTS)) {
            /* Allow passing an ascribed poly type (from ::) where rank-2 is expected. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID && args[i]->type.kind == TY_NIL) {
            /* Allow nil as a null pointer for ptr<void> parameters. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_PTR_VOID &&
            args[i]->kind == EX_INT_LIT && args[i]->as.i == 0) {
            /* Allow the `0` literal as a null pointer for a ptr<void> parameter.
             * `make-struct` of a non-parametric struct skips its field typecheck,
             * so `(make-struct Error "msg" 0)` with `cause : ptr<void>` accepts
             * the `0`-as-NULL idiom; under defstruct-as-defadt that make-struct
             * lowers to the auto-bound ctor CALL `(Error "msg" 0)`, which hits
             * this strict arg check.  Match make-struct's leniency for the
             * unambiguous NULL literal (sibling of the nil and fn ptr<void>
             * coercions above); a non-zero int still errors. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_FN && args[i]->type.kind == TY_PTR_VOID) {
            /* A#1: a fat (^fat) parameter consumes a closure in fat-box form.  A
             * capturing closure value (EX_CLOSURE, TY_PTR_VOID) is already a fat
             * box, so accept it at a fn-typed ^fat parameter -- the ^fat call
             * site fat-dispatches through slot 0.  Without this, a capturing
             * closure could not be passed to a directly-callable closure
             * parameter at all (only captureless lambda literals, which are
             * auto-shimmed via EX_FN_TO_FAT).  Gated on arg_fat so a plain fn
             * parameter still rejects a bare :ptr<void>. */
            uint32_t fn_arg_idx_fp = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_arg_idx_fp < fn_type.as.fn.arity &&
                FN_ARG_FLAG(fn_type.as.fn, fn_arg_idx_fp, FA_FAT)) {
                arg_ok = true;
            }
        }
        /* TS4P1: For a polymorphic ADT constructor, the field is stored as TY_INT
         * but its full_type is TY_TYVAR.  Accept any concrete type for such a field
         * so that e.g. (Just 1.5) at :float does not produce a type error.
         * The value will be reinterpret-cast to the concrete field type at codegen. */
        if (!arg_ok && expected_arg_kind == TY_INT) {
            uint32_t fn_arg_idx_tv = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx_tv < fn_type.as.fn.arity) {
                Type *aft2 = fn_type.as.fn.arg_full_types[fn_arg_idx_tv];
                if (aft2 && aft2->kind == TY_TYVAR) {
                    arg_ok = true;
                }
            }
        }
        if (!arg_ok && expected_arg_kind == TY_INT &&
                (args[i]->type.kind == TY_FN || args[i]->type.kind == TY_PTR_VOID)) {
            /* Phase TY5: Allow passing a function reference (TY_FN) or capturing closure
             * (TY_PTR_VOID) where int64_t is expected.  HKT typeclass method signatures
             * spell function parameters as :int (opaque int64_t); both raw function
             * pointers and fat-closure env pointers are cast to int64_t via
             * (int64_t)(intptr_t) in emit_expr.c. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_STRUCT) {
            /* Phase HKT H3: Allow passing an HKT container (TY_STRUCT) where int64_t
             * is expected.  HKT type constructor values are opaque int64_t at runtime. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_ADT) {
            /* Phase G0: ADT values are heap-allocated and passed as int64_t pointers.
             * Allow passing a TY_ADT where int64_t is expected. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_SESSION) {
            /* SS: a Session[...] channel is an opaque int64_t pointer at runtime.
             * A forward-declared callee records compound (Session ...) params as
             * the TY_INT placeholder (fwd_decl_scan_params only commits scalar
             * kinds), so a forward call such as `(loop-b ch ...)` would otherwise
             * see `expected int, got Session[...]`.  Accept it -- mirrors the
             * TY_STRUCT / TY_ADT int64_t hatches above.  Backward references,
             * where the callee's real Session signature is already installed, are
             * still checked precisely against the full protocol type. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_APP) {
            /* Phase HKT §3: Allow passing a partially-applied type (TY_APP) where int64_t
             * is expected.  Partial type application values are opaque int64_t at runtime. */
            arg_ok = true;
            /* committed-applied-arg-into-int: but not a GROUND by-value app
             * (`(Option float)`, `(Pair float int)`) into a Turmeric-bodied
             * callee whose parameter is a declared `int`.  That value is a real
             * C aggregate, not an int64 word; the call boxed it and the callee
             * read the box's ADDRESS as its integer -- `(f (some 7.1))` on
             * `[o : int]` printed 140721223092080, in both engines.  An inline-C
             * callee's `:int` is a deliberate erasure (noted below), and a
             * forward-declared callee's `int` may be a placeholder for a compound
             * parameter (no elaborated param yet), so both stay accepted. */
            uint32_t fpi = fn_binding->closure_fn_binding ? i + 1 : i;
            const FnDef *sfd = fn_binding->source_fn_def;
            if (!fn_binding->body_is_inline_c && sfd && sfd->params &&
                fpi < sfd->n_params && sfd->params[fpi] &&
                sfd->params[fpi]->type.kind == TY_INT &&
                return_type_applied_scalar_conflict(NULL, TY_INT, args[i]->type))
                arg_ok = false;
        }
        /* stdlib-region-store-hooks-unswept: a typed node handed to an inline-C
         * callee's erased `:int` parameter is an ERASURE, exactly as `(:: x :int)`
         * is -- from here the word is invisible to the region walk, and an
         * inline-C body may store it anywhere.  The explicit ascription notes
         * the word (emit_expr.c, EX_ASCRIBE); this implicit one did not, so a
         * node reaching `promise-fulfill`, `work-queue-push`, `httpd-handle`,
         * ... from inside a bracket let the generation rewind under the stored
         * pointer.  Make the coercion the same ascription, and the one note
         * covers every such store, present and future.
         *
         * A Turmeric-bodied callee is NOT exempt, which it used to be on the
         * theory that "its own stores are hooked where they happen".  They
         * are not: inside the callee the word is already an `:int`, so when
         * it forwards the word to an inline-C store the implicit-erasure rule
         * sees an int, not a node, and nothing notes it.  `sized-buf-set!` is
         * that shape -- a typed wrapper over `__sized-buf-set!-raw` -- and a
         * node stored through it read back the arena poison after the rewind
         * (security-audit-plan WP5, M-6).  For such a callee the rule is
         * narrowed to a `:heap` node word (call_arg_is_region_node_word):
         * the only thing that can be region memory, and pointer-carried, so
         * the ascription never changes representation.  That matters because
         * a callee not yet elaborated (mutual recursion) records a compound
         * parameter as the `:int` placeholder, and erasing a by-value
         * aggregate there dropped its by-ref `&temp`.  A CONSTRUCTOR is
         * excluded: its node fields arrive through `:int`-kinded slots too,
         * but the box it fills is routed into the same generation (or, when
         * malloc'd, notes its own field words), so the store is
         * region-to-region -- noting it retired every typed tree a bracket
         * built.  The known cost is `tcons`, an ordinary defn whose tail is
         * `t : int`: a list built with it inside a bracket now retires;
         * `tcons-of` (typed tail) keeps its rewind.
         *
         * NOT narrowed by a declared `#fx{}`:
         * a pure constructor retains its arguments by definition --
         * zipper-new-raw is `#fx{}` and stores `focus` into the zipper it
         * builds.  The note only ever turns a rewind into a retire, so a body
         * that does not retain the word (list-length) costs that generation's
         * saving, never correctness -- the trade the typed-parameter note at
         * inline-C body entry already makes. */
        if (arg_ok && expected_arg_kind == TY_INT && fn_binding &&
                (fn_binding->body_is_inline_c
                     ? (args[i]->type.kind == TY_ADT || args[i]->type.kind == TY_APP ||
                        args[i]->type.kind == TY_STRUCT)
                     : (call_arg_is_region_node_word(args[i]->type) &&
                        !elab_lookup_ctor(e, fn_binding->name)))) {
            uint32_t fai = fn_binding->closure_fn_binding ? i + 1 : i;
            const Type *decl = (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                                fai < fn_type.as.fn.arity)
                ? fn_type.as.fn.arg_full_types[fai] : NULL;
            if (!decl || decl->kind == TY_INT) {
                Expr *asc = expr_new(e->arena, EX_ASCRIBE, TYPE_INT, args[i]->span);
                asc->as.ascribe_.inner = args[i];
                asc->as.ascribe_.type_form = NULL;
                args[i] = asc;
            }
        }
        if (!arg_ok && expected_arg_kind == TY_APP && args[i]->type.kind == TY_ADT) {
            /* Phase HKT/G4: Allow passing a TY_ADT where TY_APP is expected.
             * Both lower to int64_t at runtime.  This arises when a function
             * parameter is annotated with a parameterised type like (Equal a b)
             * (which parses as TY_APP) and the caller passes a GADT constructor
             * value such as Refl (which has type TY_ADT). */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_APP && args[i]->type.kind == TY_TYVAR) {
            /* Phase HKT/G4: Allow passing a TY_TYVAR where TY_APP is expected.
             * Type variables and applied types share int64_t representation. */
            arg_ok = true;
        }
        /* GS2: when the callee preserved a full applied parameter type, compare
         * the full TY_APP structure rather than accepting any TY_APP argument. */
        if (arg_ok && fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
            uint32_t fn_arg_idx_app = fn_binding->closure_fn_binding ? i + 1 : i;
            Type *expected_full = (fn_arg_idx_app < fn_type.as.fn.arity)
                ? fn_type.as.fn.arg_full_types[fn_arg_idx_app] : NULL;
            if (expected_full && expected_full->kind == TY_FORALL) {
                /* van-laarhoven-generic-inference-gap (gap 1): the callee's outer
                 * type params (e.g. view's focus type `A`) may appear only inside
                 * a rank-2 forall-typed parameter.  Bind them by descending into
                 * the forall body (forall-bound vars treated as wildcards), so a
                 * generic `(view lens s)` infers its focus type from the lens
                 * instead of leaving the result an abstract tyvar.  Purely
                 * additive: `arg_ok` is left to the checks above / the rank-2
                 * EX_POLY_WRAP path, so a non-generic forall param is untouched. */
                call_collect_forall_outer_bindings(
                    expected_full, args[i]->type, type_bindings, &n_type_bindings);
            } else if (expected_full && call_type_has_named_tyvar(expected_full)) {
                {
                    const Elab *saved_cb_elab = g_call_cb_elab;
                    g_call_cb_elab = e;
                    arg_ok = call_collect_type_bindings(expected_full, args[i]->type,
                                                        type_bindings, &n_type_bindings);
                    g_call_cb_elab = saved_cb_elab;
                }
                /* open-arg-first-binds-call-result-as-int: a return-only
                 * generic call (`(none)`) that no sibling has grounded yet
                 * bound the variable provisionally; ground it once every
                 * argument is in (below the loop). */
                if (arg_ok && expected_full->kind == TY_APP &&
                    w2_arg_is_free_poly_call(args[i]) &&
                    !ng_type_names_sig_tyvar(e, &args[i]->type)) {
                    Type inst_now = call_instantiate_type(e, expected_full,
                                                          type_bindings, n_type_bindings);
                    if (call_type_has_named_tyvar(&inst_now)) {
                        if (!open_first) {
                            open_first = (bool *)arena_alloc(
                                e->arena, (n_args ? n_args : 1) * sizeof(bool));
                            open_first_strict = (bool *)arena_alloc(
                                e->arena, (n_args ? n_args : 1) * sizeof(bool));
                            for (uint32_t oi = 0; oi < n_args; oi++)
                                open_first[oi] = open_first_strict[oi] = false;
                        }
                        open_first[i] = true;
                    }
                }
                /* nullary-generic-call-under-tyvar-expectation: `(wrap 3
                 * (box-nil))` against `[v : A b : (Box A)]`, or `(make-struct
                 * W 8 (none))` against `(opt (Option A))`.  Arg 1 bound
                 * `A := int`; the collect above then compares that binding
                 * with the argument's OWN open tyvar (`(Box 'A)` from the
                 * nullary call's result) and reports `expected (Box A), got
                 * (Box A)`.  The argument is a return-only polymorphic call
                 * result -- exactly W2's population below -- so instantiate
                 * the parameter with the bindings the siblings produced and,
                 * when that grounds it, unify the argument's tyvar against it
                 * the way W2 does for a concrete parameter: record the
                 * substitution on the nullary call so emit monomorphizes it
                 * (`none__spec__Option__int`), and take the grounded type as
                 * the argument's.  A parameter the siblings leave open keeps
                 * the ordinary rejection. */
                if (!arg_ok && expected_full->kind == TY_APP &&
                    args[i]->type.kind == TY_APP &&
                    ng_type_names_sig_tyvar(e, &args[i]->type)) {
                    /* M7: the argument names the ENCLOSING body's abstract
                     * tyvar (a recursive call's `(Cons A)`, an ascription to
                     * it) -- not a fresh one.  It stays abstract: match it
                     * against the instantiated parameter with the erased
                     * carrier standing in for `A` (ng_erased_match), record
                     * nothing, and never pin the call to a sibling's erased
                     * `int`, which would monomorphize a generic body's
                     * recursion at the wrong element type (a float list read
                     * back int bits). */
                    Type inst = call_instantiate_type(e, expected_full,
                                                      type_bindings, n_type_bindings);
                    if (ng_erased_match(e, &inst, &args[i]->type)) arg_ok = true;
                } else if (!arg_ok && expected_full->kind == TY_APP &&
                    args[i]->type.kind == TY_APP &&
                    w2_arg_is_free_poly_call(args[i])) {
                    Type inst = call_instantiate_type(e, expected_full,
                                                      type_bindings, n_type_bindings);
                    if (inst.kind == TY_APP && !call_type_has_named_tyvar(&inst)) {
                        CallTypeBinding ngscratch[16];
                        uint8_t ngn = 0;
                        if (call_collect_type_bindings(&args[i]->type, inst,
                                                       ngscratch, &ngn)) {
                            if (ngn > 0 && !args[i]->as.call_.abi_bindings) {
                                AbiTypeBinding *saved = (AbiTypeBinding *)arena_alloc(
                                    e->arena, ngn * sizeof(AbiTypeBinding));
                                for (uint8_t bi = 0; bi < ngn; bi++) saved[bi] = ngscratch[bi];
                                args[i]->as.call_.abi_bindings   = saved;
                                args[i]->as.call_.n_abi_bindings = ngn;
                            }
                            args[i]->type = inst;
                            arg_ok = true;
                        }
                    } else if (inst.kind == TY_APP && i + 1 < n_args &&
                               !ng_type_names_sig_tyvar(e, &args[i]->type)) {
                        /* open-arg-first-binds-call-result-as-int: the
                         * siblings that would ground this parameter come
                         * LATER -- `(cx (map-new) 7.1)` against `[m : (Map int
                         * A) d : A]`, whose `K` met the concrete `int` before
                         * anything bound `A`.  Accept it for now; the pass
                         * below the loop grounds it, and reports this same
                         * mismatch if it still does not fit. */
                        if (!open_first) {
                            open_first = (bool *)arena_alloc(
                                e->arena, (n_args ? n_args : 1) * sizeof(bool));
                            open_first_strict = (bool *)arena_alloc(
                                e->arena, (n_args ? n_args : 1) * sizeof(bool));
                            for (uint32_t oi = 0; oi < n_args; oi++)
                                open_first[oi] = open_first_strict[oi] = false;
                        }
                        open_first[i] = true;
                        open_first_strict[i] = true;
                        arg_ok = true;
                    }
                }
            } else if (arg_ok && expected_arg_kind == TY_APP &&
                       args[i]->type.kind == TY_APP &&
                       expected_full && expected_full->kind == TY_APP) {
                arg_ok = type_eq(args[i]->type, *expected_full);
                /* W2 (fresh empty-container forward unification): the structural
                 * type_eq above rejects a bare (vec-new) : (Vec A) handed straight
                 * to a concrete (Vec int) parameter (A != int), even though the
                 * empty container soundly inhabits (Vec int).  S4/#461 only
                 * back-propagates the element type onto a let-bound vec-push!
                 * receiver pinned by a sibling argument; a fresh (vec-new) with no
                 * pusher leaves the element tyvar open.  When the argument is a
                 * return-only polymorphic call result (no argument of the call
                 * carries the tyvar -- so it is genuinely parametric, not an
                 * abstract (Vec A) parameter the body must keep abstract), unify
                 * the argument's tyvar against the concrete parameter and accept.
                 * A scratch binding set keeps the callee's own type_bindings (and
                 * thus its carrier/relay emission) untouched. */
                if (!arg_ok && !call_type_has_named_tyvar(expected_full) &&
                    call_type_has_named_tyvar(&args[i]->type) &&
                    w2_arg_is_free_poly_call(args[i])) {
                    CallTypeBinding w2scratch[16];
                    uint8_t w2n = 0;
                    if (call_collect_type_bindings(&args[i]->type, *expected_full,
                                                   w2scratch, &w2n)) {
                        /* Record the element substitution (e.g. A := int) on the
                         * (vec-new) call itself so emit monomorphizes it to the
                         * concrete specialization (vec_new__spec__Vec__int__)
                         * rather than passing the int64 carrier straight into a
                         * Vec__int* parameter -- matching the clean codegen the
                         * (:: (vec-new) (Vec int)) ascription produces and avoiding
                         * a -Wint-conversion carrier-to-pointer warning.  Only set
                         * it when the call carries no bindings of its own. */
                        if (w2n > 0 && !args[i]->as.call_.abi_bindings) {
                            AbiTypeBinding *saved = (AbiTypeBinding *)arena_alloc(
                                e->arena, w2n * sizeof(AbiTypeBinding));
                            for (uint8_t bi = 0; bi < w2n; bi++) saved[bi] = w2scratch[bi];
                            args[i]->as.call_.abi_bindings   = saved;
                            args[i]->as.call_.n_abi_bindings = w2n;
                        }
                        args[i]->type = *expected_full;
                        arg_ok = true;
                    }
                }
            } else if (arg_ok &&
                       (expected_arg_kind == TY_REF_IMMUT || expected_arg_kind == TY_REF_MUT) &&
                       expected_full && expected_full->kind == expected_arg_kind &&
                       expected_full->as.ref_borrow.target_full) {
                /* borrowed-aggregate-key-skips-the-key-check: a concrete
                 * `(& Pt)` parameter matched any borrow of an ADT by kind, so
                 * `(& q)` for a Qt -- or a String -- was accepted.  type_eq
                 * compares the recorded targets (and falls back to the kind
                 * when the argument has none). */
                arg_ok = type_eq(args[i]->type, *expected_full);
            }
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_FN) {
            /* Phase HKT H3/H4: Allow passing a function value where int64_t is expected.
             * Function references are represented as int64_t in HKT helper calls. */
            arg_ok = true;
        }
        if (!arg_ok && expected_arg_kind == TY_INT && args[i]->type.kind == TY_PTR_VOID) {
            /* Phase HKT §5: Allow passing a capturing closure (heap-allocated env struct,
             * TY_PTR_VOID) where int64_t is expected.  emit.c will apply the
             * (int64_t)(intptr_t) cast so the generated C99 code is valid. */
            arg_ok = true;
        }
        /* IT4: Union type subtyping — accept a value of type A where (A | B) is expected.
         * Wrap the argument with EX_UNION_INJECT so emit.c produces TUR_TAG(idx, val). */
        if (!arg_ok && expected_arg_kind == TY_UNION) {
            uint32_t fn_arg_idx3 = i;
            if (fn_binding->closure_fn_binding) fn_arg_idx3 = i + 1;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx3 < fn_type.as.fn.arity) {
                Type *union_t = fn_type.as.fn.arg_full_types[fn_arg_idx3];
                if (union_t && union_t->kind == TY_UNION) {
                    for (uint8_t um = 0; um < union_t->as.union_.n_members; um++) {
                        Type *mem = union_t->as.union_.members[um];
                        if (mem && type_eq(args[i]->type, *mem)) {
                            arg_ok = true;
                            /* IT4: wrap in EX_UNION_INJECT to tag the value at runtime */
                            Expr *inject = expr_new(e->arena, EX_UNION_INJECT,
                                                    *union_t, args[i]->span);
                            inject->as.union_inject_.tag_idx = (int64_t)um;
                            inject->as.union_inject_.value = args[i];
                            /* union-tagged-union-c-emission: the frame-box rule,
                             * verbatim from the `any` widen below -- same widen,
                             * same tur_tagged_t, same unowned malloc when the
                             * member is a by-value aggregate.  See the comment
                             * there for why each of the three conditions is
                             * required; all three only ever DECLINE, so an
                             * unmodelled shape keeps the status-quo allocation.
                             *
                             * The extra condition here is the closure guard: the
                             * mask is indexed by the CALLEE's own parameter
                             * vector, and this branch is one of several at this
                             * call site that shift by one for a closure's env
                             * parameter.  Rather than decide which convention the
                             * mask follows, refuse a closure callee outright --
                             * a wrong bit here would hand a callee that retains
                             * the payload a pointer into a frame that is about to
                             * die, turning a leak into a dangling pointer. */
                            if (fn_binding && !fn_binding->closure_fn_binding &&
                                i < 32 &&
                                (fn_binding->nonretain_ptr_param_mask & (1u << i)) &&
                                fn_binding->type.kind == TY_FN &&
                                effect_row_is_empty(fn_binding->type.as.fn.effect_row))
                                inject->as.union_inject_.frame_box = true;
                            args[i] = inject;
                            break;
                        }
                    }
                    /* Also accept if the argument is already the same union type (no injection needed) */
                    if (!arg_ok && args[i]->type.kind == TY_UNION) {
                        arg_ok = type_eq(args[i]->type, *union_t);
                    }
                }
            }
        }
        /* IT1: Widening — accept a member type where the expected union matches. */
        if (!arg_ok && args[i]->type.kind == TY_UNION &&
            expected_arg_kind == TY_UNION) {
            arg_ok = (args[i]->type.kind == expected_arg_kind);
        }
        /* IT3: Intersection elimination — accept a value of intersection type (A & B)
         * where any single member type is expected.  (A & B) <: A and (A & B) <: B. */
        if (!arg_ok &&
            args[i]->type.kind == TY_INTERSECTION) {
            Type *isect_t = &args[i]->type;
            for (uint8_t im = 0; im < isect_t->as.intersection_.n_members; im++) {
                Type *mem = isect_t->as.intersection_.members[im];
                if (mem && mem->kind == expected_arg_kind) {
                    arg_ok = true;
                    break;
                }
            }
        }
        /* IT3: Intersection introduction check — function expects (A & B), arg must
         * satisfy all members.  Emit TUR_E0351 for the first unsatisfied member. */
        if (!arg_ok && expected_arg_kind == TY_INTERSECTION) {
            uint32_t fn_arg_idx5 = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx5 < fn_type.as.fn.arity) {
                Type *isect_t = fn_type.as.fn.arg_full_types[fn_arg_idx5];
                if (isect_t && isect_t->kind == TY_INTERSECTION) {
                    /* Check each member -- the arg must match all of them */
                    bool all_ok = true;
                    Type first_mismatch = TYPE_UNKNOWN;
                    bool have_mismatch = false;
                    for (uint8_t im = 0; im < isect_t->as.intersection_.n_members; im++) {
                        Type *mem = isect_t->as.intersection_.members[im];
                        if (!mem) continue;
                        /* For concrete member types, the arg must have an equal or
                         * compatible type.  For typeclass members we cannot yet do
                         * instance resolution here, so skip them. */
                        if (!typekind_is_concrete_for_disjoint(mem->kind)) continue;
                        if (!type_eq(args[i]->type, *mem)) {
                            all_ok = false;
                            first_mismatch = *mem;
                            have_mismatch = true;
                            break;
                        }
                    }
                    if (all_ok) {
                        arg_ok = true;
                    } else {
                        /* PH2.2: build composite type names into owned buffers
                         * (see PH2.1) so this error path does not leak. */
                        Buf got_buf; buf_init(&got_buf);
                        type_print(&got_buf, args[i]->type);
                        buf_putc(&got_buf, '\0');
                        Buf mem_buf; buf_init(&mem_buf);
                        if (have_mismatch) type_print(&mem_buf, first_mismatch);
                        else buf_puts(&mem_buf, "?");
                        buf_putc(&mem_buf, '\0');
                        diag_emit_with_code(DIAG_ERROR, args[i]->span,
                            TUR_E0351_INTERSECTION_MEMBER_MISMATCH,
                            "function '%s' arg %u: value of type %s does not satisfy "
                            "intersection member %s",
                            fn_binding->name->name, i + 1,
                            got_buf.data,
                            mem_buf.data);
                        buf_free(&got_buf);
                        buf_free(&mem_buf);
                        return NULL;
                    }
                }
            }
        }

        /* IT4: A <: any — any value satisfies the top type.
         * Wrap with EX_UNION_INJECT (via the shared coercion helper) using the
         * TypeKind of the value as the tag, so (type-of) and (cast) can retrieve
         * it at runtime.  TY2.2: by-value structs are heap-boxed by the helper. */
        if (!arg_ok && expected_arg_kind == TY_ANY) {
            arg_ok = true;
            args[i] = elab_coerce_to_any(e, args[i]);
            /* any-struct-box-leak-per-widen: a by-value payload widened here is
             * heap-copied by the emitter, and nothing owns that copy -- one
             * malloc leaked per widen, which a loop turns into linear growth.
             * When the callee provably cannot keep the payload past this call,
             * the copy belongs in the CALLER's frame instead, and then there is
             * no allocation to own.  Three conditions, each of which only ever
             * declines:
             *
             *   direct callee   -- given: elab_call_fn_inner is the
             *                      statically-known-callee path, so an indirect
             *                      call (no body to inspect) never arrives here;
             *   non-retaining   -- the inferred mask (elab_fns.c) says the body
             *                      does not keep a pointer this parameter
             *                      carries, and its result cannot carry one out.
             *                      Cleared for any body containing inline-C,
             *                      which could stash the pointer where no AST
             *                      walk can see it;
             *   effect-free     -- a callee that can PERFORM may suspend, and
             *                      its resumption must not reach back into a
             *                      caller frame that the trampoline has left.
             *                      A declared row variable reads as non-empty
             *                      and is refused, so this is conservative.
             *
             * Same soundness posture as the closure-env and catch-box frees this
             * inference already serves: it only ever greenlights, and every
             * unmodelled shape keeps the status-quo allocation. */
            if (args[i] && args[i]->kind == EX_UNION_INJECT &&
                fn_binding && i < 32 &&
                (fn_binding->nonretain_ptr_param_mask & (1u << i)) &&
                fn_binding->type.kind == TY_FN &&
                effect_row_is_empty(fn_binding->type.as.fn.effect_row)) {
                args[i]->as.union_inject_.frame_box = true;
                /* any-cannot-recover-a-capturing-closure: the same fact frees
                 * the fat SHIM box a fn payload is wrapped in.  frame_box says
                 * the callee neither retains this payload nor can suspend, so a
                 * copy may live in the caller's frame -- and a shim box under
                 * exactly those conditions may live there too, which is what
                 * `stack_ok` asks for.  Without it a widen of a fn value the
                 * emitter cannot hoist to a static box (a let-bound lambda: its
                 * binding is local, so the box is not a link-time constant)
                 * mallocs one per call that nothing frees -- measured at 100
                 * boxes over 100 iterations. */
                if (args[i]->as.union_inject_.value &&
                    args[i]->as.union_inject_.value->kind == EX_FN_TO_FAT)
                    args[i]->as.union_inject_.value->as.fn_to_fat_.stack_ok = true;
            }
        }

        /* any-struct-box-leak-per-widen (the temporary case): the argument was
         * ALREADY an `any` -- no widen here to frame-box -- but it is a fresh
         * one this expression owns and nothing else does: the result of a call
         * whose body's tail is a widen (returns_fresh_any).  Its payload box has
         * no owner anywhere, so without this it leaks once per evaluation.
         *
         * The same two conditions as the frame-box rule make it safe to drop as
         * soon as the consuming call returns -- the callee does not keep it, and
         * cannot suspend between receiving it and returning.  What is NOT enough
         * is "the argument is a call returning any": a callee that hands back an
         * `any` it was GIVEN (`(defn keep-it [v : any] : any v)`) returns an
         * alias, and dropping that would free a box its other holder still uses.
         * returns_fresh_any is exactly the distinction, and it is why this needs
         * a callee-side fact rather than a call-site shape. */
        if (expected_arg_kind == TY_ANY && args[i] && i < 32 && fn_binding
            && (fn_binding->nonretain_ptr_param_mask & (1u << i))
            && fn_binding->type.kind == TY_FN
            && effect_row_is_empty(fn_binding->type.as.fn.effect_row)) {
            if (any_expr_is_owned_temp(args[i], 8))
                args[i]->any_drop_after = true;
        }

        /* RM1 (reclamation-plan): the sum-box temporary case, the any rule one
         * type over.  `(ok? (ok 1))` mallocs a carrier box on the erased path
         * that nothing owns -- the sweep's dominant leak shape.  Stamp the
         * argument call when the CONSUMER is a read-only accessor (argument 0,
         * pure -- the same non-suspending condition the any rule carries) and
         * the PRODUCER's every value path mints a fresh box (the callee-side
         * freshness fact; a pass-through like alt-or stays unstamped, exactly
         * as `keep-it` does for any).  Emit frees the temp after the accessor
         * call materializes, gated there on the temp's recorded int64 spelling
         * so a specialized by-value call is never touched. */
        if (args[i] && fn_binding && fn_binding->name &&
            fn_binding->type.kind == TY_FN &&
            effect_row_is_empty(fn_binding->type.as.fn.effect_row) &&
            ((sum_box_reader_name(fn_binding->name->name) &&
              /* Which argument POSITIONS hold a readable box differs per
               * callee, and the restriction is load-bearing: unwrap-or's arg 1
               * is the DEFAULT, which the callee RETURNS on the None path --
               * stamping it would free a value the caller receives.  The eq?
               * comparators read boxes at 0 and 1; everything else at 0 only. */
              (i == 0 ||
               (i == 1 && (strcmp(fn_binding->name->name, "result-eq?") == 0 ||
                           strcmp(fn_binding->name->name, "option-eq?") == 0)))) ||
             /* value-struct-payload-sum-monomorph-box-has-no-owner: or a USER
              * callee whose body was inferred not to retain this sum-typed
              * parameter (and whose result cannot carry it out) -- the same
              * fact the name allowlist asserts, checked instead of trusted.
              * Zero for inline-C bodies and dictionary-dispatched methods. */
             (i < 32 && (fn_binding->nonretain_sum_param_mask & (1u << i))))) {
            Expr *a = args[i];
            while (a && a->kind == EX_ASCRIBE) a = a->as.ascribe_.inner;
            /* A dictionary dispatch (abstract receiver / return type) is
             * stamped TENTATIVELY: the callee that runs is only known per
             * monomorph at emit, which re-resolves it and asks the freshness
             * question of that binding before freeing anything. */
            if (call_returns_fresh_sum_box(a) ||
                (a && a->kind == EX_CALL && a->as.call_.dict_arg &&
                 !call_dispatch_is_static(a)))
                a->sum_box_drop_after = true;
        }

        /* plain-fn-typed-params-are-kind-matched: a plain `(fn [int] int)`
         * parameter is routed onto the typed poly carrier (elab_fns.c F5), so
         * its arg_kinds slot reads TY_PTR_VOID and the LT2 gate below -- which
         * asks for TY_FN on both sides -- never ran the structural check: a
         * `(fn [cstr] cstr)` or a wrong-arity lambda was accepted, exit 0.
         * The declared signature is right there in arg_full_types (the F5
         * route stores it); check it the same way LT2 checks a ^fat slot. */
        if (arg_ok && args[i]->type.kind == TY_FN &&
            expected_arg_kind != TY_FN && fn_type.kind == TY_FN &&
            fn_type.as.fn.arg_full_types) {
            uint32_t pidx = fn_binding->closure_fn_binding ? i + 1 : i;
            const Type *declared_fn = pidx < fn_type.as.fn.arity
                ? fn_type.as.fn.arg_full_types[pidx] : NULL;
            if (declared_fn && declared_fn->kind == TY_FN &&
                !fn_type_structurally_compatible(args[i]->type, *declared_fn)) {
                Buf sx; buf_init(&sx);
                type_print(&sx, *declared_fn); buf_putc(&sx, '\0');
                Buf sa; buf_init(&sa);
                type_print(&sa, args[i]->type); buf_putc(&sa, '\0');
                diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                    TUR_E0001_TYPE_MISMATCH,
                                    "function '%s' arg %u: expected a function of type %s, "
                                    "got %s -- arity, argument types and result type must match",
                                    fn_binding->name->name, i + 1, sx.data, sa.data);
                buf_free(&sx); buf_free(&sa);
                return NULL;
            }
        }

        /* LT2: When both expected and actual argument types are function types,
         * verify that their arg_linear flags match.  This catches attempts to
         * pass a (-> T R) function where (-> ^linear T R) is required (or vice
         * versa) in higher-order call positions. */
        /* concrete-result-fn-passed-where-an-any-result-fn-is-expected: a
         * function argument whose signature differs from the parameter's
         * only in where `any` appears -- `(fn [float float] float)` for a
         * `(fn [float float] any)` -- is a different calling convention, not
         * a subtype.  The callee would call it through the parameter's
         * signature and read a `tur_tagged_t` out of `rax:rdx` where the
         * function left a double in `xmm0`.  Marshal it with the `any`
         * bridge.  Only a ground parameter type is bridged (a generic one is
         * instantiated from this very argument). */
        if (arg_ok &&
            expected_arg_kind == TY_FN && args[i]->type.kind == TY_FN &&
            fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
            uint32_t bidx = fn_binding->closure_fn_binding ? i + 1 : i;
            const Type *bexp = (bidx < fn_type.as.fn.arity)
                ? fn_type.as.fn.arg_full_types[bidx] : NULL;
            if (bexp && bexp->kind == TY_FN) {
                Expr *ad = elab_fn_any_bridge(e, args[i], bexp, NULL, 0);
                if (ad) args[i] = ad;
            }
        }
        if (arg_ok &&
            expected_arg_kind == TY_FN && args[i]->type.kind == TY_FN) {
            uint32_t fn_arg_idx_lt2 = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx_lt2 < fn_type.as.fn.arity) {
                const Type *expected_fn = fn_type.as.fn.arg_full_types[fn_arg_idx_lt2];
                if (expected_fn && expected_fn->kind == TY_FN &&
                    !fn_type_structurally_compatible(args[i]->type, *expected_fn)) {
                    Buf sx; buf_init(&sx);
                    type_print(&sx, *expected_fn); buf_putc(&sx, '\0');
                    Buf sa; buf_init(&sa);
                    type_print(&sa, args[i]->type); buf_putc(&sa, '\0');
                    diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                        TUR_E0001_TYPE_MISMATCH,
                                        "function '%s' arg %u: expected a function of type %s, "
                                        "got %s -- arity, argument types and result type must match",
                                        fn_binding->name->name, i + 1, sx.data, sa.data);
                    buf_free(&sx); buf_free(&sa);
                    return NULL;
                }
                if (expected_fn && expected_fn->kind == TY_FN &&
                    !fn_type_subtype(args[i]->type, *expected_fn)) {
                    /* PH2.2: owned buffer for the composite (fn) type name. */
                    Buf exp_buf; buf_init(&exp_buf);
                    type_print(&exp_buf, *expected_fn);
                    buf_putc(&exp_buf, '\0');
                    diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                        TUR_E0001_TYPE_MISMATCH,
                                        "function '%s' arg %u: linear function type mismatch"
                                        " -- expected %s but got a function with different"
                                        " linearity annotations; ^linear parameters must match exactly",
                                        fn_binding->name->name, i + 1,
                                        exp_buf.data);
                    buf_free(&exp_buf);
                    return NULL;
                }
            }
        }

        /* PH1.2: Row-precise handler argument checking. When both expected and
         * actual argument types are handlers, the kind-only `arg_ok` above is
         * not enough -- any handler would satisfy any handler parameter. Refine
         * it with `type_is_subtype` (PH0.2) so the handled-effect row and
         * value/result kinds must be compatible (FH4.1 relation: set-equality +
         * TY_UNKNOWN wildcards). The declared handler type is threaded into
         * arg_full_types by PH1.1. */
        if (arg_ok &&
            expected_arg_kind == TY_HANDLER && args[i]->type.kind == TY_HANDLER) {
            uint32_t fn_arg_idx_h = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                fn_arg_idx_h < fn_type.as.fn.arity) {
                Type *expected_h = fn_type.as.fn.arg_full_types[fn_arg_idx_h];
                if (expected_h && expected_h->kind == TY_HANDLER) {
                    arg_ok = type_is_subtype(args[i]->type, *expected_h);
                }
            }
        }

        if (!arg_ok && ms_lenient) {
            /* defstruct-as-defadt (exg5-exists-cycle): this is the make-struct ->
             * ctor-call rewrite of a NON-parametric record ADT.  Default
             * make-struct does no field typecheck, so accept the value as-is to
             * preserve parity (e.g. `0`/NULL ptr<void> into an rc<T>/ptr<T>
             * field).  ms_lenient was cleared at entry, so this only relaxes the
             * direct ctor args, never anything nested. */
            arg_ok = true;
        }
        /* r7rs-dead-mistyped-call-refused-at-compile-time: in a `#lang r7rs`
         * file a statically mistyped argument is not a compile-time error.
         * R7RS makes `(car 5)` an error only when it is EVALUATED (1.3.2), so
         * `(let ((x #f)) (if x (car x) 0))` -- and SRFI 2's `(and-let* ((x #f)
         * (y (car x))) y)` -- are valid programs that never run the call.
         * Widen the argument to `any` and let the seam just below insert its
         * checked unbox: a call that does run fails there, and that failure
         * raises an R7RS error object (r7rs-type-errors-are-uncatchable-panics).
         * Scheme files only: Saffron keeps its static refusal.  A borrow on
         * either side is left to the borrow seam further down. */
        if (!arg_ok && args[i] && args[i]->type.kind != TY_ANY &&
            lang_span_is_scheme(call->span) &&
            expected_arg_kind != TY_ANY && expected_arg_kind != TY_TYVAR &&
            expected_arg_kind != TY_UNKNOWN &&
            expected_arg_kind != TY_REF_IMMUT && expected_arg_kind != TY_REF_MUT &&
            args[i]->type.kind != TY_REF_IMMUT && args[i]->type.kind != TY_REF_MUT) {
            Expr *widened = elab_coerce_to_any(e, args[i]);
            if (widened) args[i] = widened;
        }
        /* saffron-lang-plan D5/S4: the Saffron -> Turmeric seam.
         *
         * A Saffron caller holds `any` and the callee has a real signature.
         * D5 weighed three options and chose gradual typing with a RUNTIME
         * CHECK at the seam: rejecting unless the caller annotates is safe and
         * unusable, and erasing -- passing the payload word through unchecked
         * -- turns a type error into a memory-safety bug, because the word is
         * whatever the box held.
         *
         * The checked unbox already exists: `elab_any_unbox_to` is the node
         * `(cast x T)` lowers to, so a wrong type panics with the ordinary
         * `cast: any holds ...` message rather than reinterpreting the payload.
         * That is exactly the contract this seam wants, so the seam is that
         * node inserted at the argument rather than a new mechanism.
         *
         * Only for a CONCRETE expected type: a callee expecting `any` needs no
         * check, and one expecting a type variable has nothing to check
         * against. */
        /* saffron-dynamic-surface-pass M10: the CALL's span counts too.  A
         * macro-expanded argument -- `(s (map-get m k))`, every `map-*`
         * accessor is a macro -- carries stdlib/map.tur's span, so gating on
         * the argument alone turned a Saffron call into a static
         * "expected int, got any" reported inside the stdlib. */
        if (!arg_ok && args[i] && args[i]->type.kind == TY_ANY &&
            (lang_span_is_dynamic(args[i]->span) ||
             lang_span_is_dynamic(call->span) || e->toplevel_dynamic) &&
            expected_arg_kind != TY_ANY && expected_arg_kind != TY_TYVAR &&
            expected_arg_kind != TY_UNKNOWN) {
            Type want = type_from_kind(expected_arg_kind);
            const Type *want_decl = NULL;
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
                uint32_t fi = fn_binding->closure_fn_binding ? i + 1 : i;
                Type *ct = (fi < fn_type.as.fn.arity)
                    ? fn_type.as.fn.arg_full_types[fi] : NULL;
                if (ct) { want = *ct; want_decl = ct; }
            }
            /* saffron-dynamic-surface-pass H9: before grounding, bind what the
             * OTHER arguments already determine.  `(map-get m k)` with an
             * any-held `m` and a `Sym` key expands to `(tur-map-kcheck m (& k))`
             * whose `(Map K V)` seam ran on argument 0 before the key had
             * bound K, so K and V were both grounded to `any` and the check
             * failed as `holds a different instantiation of Map` against the
             * literal's `(Map Sym any)`.  A one-shot collection over the
             * concrete siblings gives K := Sym; only the genuinely open
             * arguments are grounded. */
            if (want_decl && fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types &&
                call_type_has_named_tyvar(want_decl)) {
                CallTypeBinding pre[16];
                uint8_t n_pre = 0;
                for (uint32_t j = 0; j < n_args; j++) {
                    if (j == i) continue;
                    /* A LATER sibling is not elaborated yet (arguments are
                     * elaborated in order).  Elaborate it here when its form
                     * needs no expected-type steering -- a symbol, a literal,
                     * or a `(& sym)` borrow, which is what the map macros
                     * pass -- and mark it done so the main loop reuses it,
                     * as the bidirectional-inference path above already
                     * does for a fn-typed parameter. */
                    if (!args[j] && !arg_done[j] && 1 + j < call->as.list.len) {
                        const Form *sf = call->as.list.items[1 + j];
                        bool simple = sf->tag == F_SYM || sf->tag == F_INT ||
                                      sf->tag == F_FLOAT || sf->tag == F_STR ||
                                      sf->tag == F_KEYWORD ||
                                      (sf->tag == F_LIST && sf->as.list.len == 2 &&
                                       sf->as.list.items[0]->tag == F_SYM &&
                                       sf->as.list.items[0]->as.sym == e->sym_ampersand &&
                                       sf->as.list.items[1]->tag == F_SYM);
                        if (simple) {
                            args[j] = elab_form(e, (Form *)sf);
                            arg_done[j] = true;
                        }
                    }
                    if (!args[j] || args[j]->type.kind == TY_ANY) continue;
                    /* saffron-open-generic-result-not-grounded: only a
                     * CONTAINER sibling pins the instantiation (a typed map
                     * passed beside this one).  A key or element does not:
                     * every container a dynamic file builds is the all-`any`
                     * instantiation, so binding K := Sym from `(map-get m :a)`
                     * checked a `(Map any any)` box against `(Map Sym any)`
                     * and panicked -- the same shape as `(vec-push! v "x")`
                     * on an `any`-held vector.  Left open, K grounds to `any`
                     * below, and the scalar sibling is widened to meet it
                     * (by value by the widen seam, through a borrow by the
                     * borrow seam). */
                    if (args[j]->type.kind != TY_APP) continue;
                    uint32_t fj = fn_binding->closure_fn_binding ? j + 1 : j;
                    Type *ej = (fj < fn_type.as.fn.arity)
                        ? fn_type.as.fn.arg_full_types[fj] : NULL;
                    /* A bare tyvar parameter (`k : K`) has no full type on
                     * the fn type -- arg_full_types is for compound args --
                     * but the callee's own FnDef keeps the named tyvar. */
                    if (!ej && fn_binding->source_fn_def &&
                        fn_binding->source_fn_def->params &&
                        fj < fn_binding->source_fn_def->n_params &&
                        fn_binding->source_fn_def->params[fj])
                        ej = &fn_binding->source_fn_def->params[fj]->type;
                    if (!ej || !call_type_has_named_tyvar(ej)) continue;
                    (void)call_collect_type_bindings(ej, args[j]->type, pre, &n_pre);
                }
                if (n_pre > 0) want = call_instantiate_type(e, &want, pre, n_pre);
            }
            /* saffron-unannotated-param-container-cast-panics: ground the
             * target's OPEN type arguments before checking against it.  See
             * call_ground_open_app_args_to_any -- `(Vec A)` is a TY_APP, so the
             * exemption above (which tests the KIND) never fired for it, and
             * the seam checked against an instantiation nobody chose. */
            bool grounded = call_ground_open_app_args_to_any(e->arena, &want);
            /* saffron-dynamic-surface-pass H7: a FUNCTION target needs
             * marshalling rather than a checked unbox -- see
             * `saffron_seam_fn_adaptor`.  It declines (NULL) for a target that
             * is already all-`any`, or one it cannot spell, and the plain
             * unbox below then runs exactly as before. */
            Expr *seam_fn_ad = NULL;
            if (want.kind == TY_FN && 1 + i < call->as.list.len)
                seam_fn_ad = saffron_seam_fn_adaptor(e, call->as.list.items[1 + i],
                                                     &want, args[i]->span);
            /* all-any-fn-param-is-unusable (repro 2): the adaptor declines an
             * all-`any` target because the box already holds exactly that
             * representation -- H8's outbound adaptor makes every boxed fn an
             * all-`any` FAT closure, stamped with the boxed ("closure...") id.
             * The plain unbox below then checked the payload against the BARE
             * `(fn [] any)` id and, on the id that never matched, panicked
             * `any holds a function this cast cannot accept`; had it matched,
             * the following fat-normalising shim would have wrapped the fat
             * box as a bare code pointer.  Unbox to the boxed twin instead: the
             * cast keys on the id the widen stamped, and the value flows on as
             * the fat handle it is. */
            if (!seam_fn_ad && want.kind == TY_FN && !want.as.fn.boxed &&
                !want.as.fn.cfnptr && fn_type_is_all_any(&want))
                want.as.fn.boxed = true;
            Expr *unboxed = seam_fn_ad ? seam_fn_ad
                                       : elab_any_unbox_to(e, args[i], want,
                                                           args[i]->span);
            if (unboxed) {
                elab_any_cast_note_callee(e, unboxed, fn_binding);
                args[i] = unboxed; arg_ok = true;
                /* ...and the half that grounding ALONE does not buy.
                 *
                 * The tyvar bindings for this call were collected further up
                 * this same iteration, from the argument's type BEFORE the
                 * seam replaced it -- i.e. from `any`, which binds nothing.
                 * So `A` stayed open, `call_result_type`'s bare-tyvar collapse
                 * took the result down to the int64 carrier, and the return
                 * position then widened the element's BOX POINTER into an
                 * `any` instead of reading through it.  That is a WRONG ANSWER
                 * (the program printed 94242104267456 for 7), which is why the
                 * first attempt at this fix was reverted rather than shipped.
                 *
                 * Re-collect against the substituted argument so `A := any` is
                 * recorded and the result types as `any`, which is what the
                 * caller's Saffron context wants anyway.  Guarded on `grounded`
                 * so a seam over an already-concrete target (`(Vec int)`) keeps
                 * the bindings it had. */
                if (grounded && want_decl && !seam_fn_ad)
                    (void)call_collect_type_bindings(want_decl, args[i]->type,
                                                     type_bindings,
                                                     &n_type_bindings);
            }
        }
        /* saffron-dynamic-surface-pass (low): the WIDEN direction of the seam
         * above.  `(map-assoc #map{:a 1} :c 7.1)` in a Saffron file was
         * `function 'map-assoc-eq-o' arg 4: expected tyvar, got float`,
         * reported inside stdlib/map.tur -- because the value parameter is the
         * bare tyvar `V`, and the seam above handles only the other direction:
         * an `any` ARGUMENT meeting a CONCRETE parameter.
         *
         * Widening every concrete argument at a tyvar parameter would be
         * wrong.  `V` may be bound to something real by another argument, and
         * a `(Map Sym int)` built in typed code must keep rejecting a float
         * even when the call is written in a Saffron file.  So bind `V` from
         * the SIBLINGS first, exactly as H9 does above, and widen only when
         * what they determine is `any` -- which is what a Saffron container's
         * element type always is.  Bind nothing, or bind it to anything else,
         * and the ordinary error stands unchanged.
         *
         * The binding sibling here is an EARLIER argument (the map itself), so
         * unlike H9 this needs no elaborate-a-later-sibling step: `args[j]` is
         * already there.
         *
         * This is the call-position twin of `dl_saffron_widen_elem`, which
         * widens a data LITERAL's elements before the homogeneous `vec-of`
         * macro sees them -- same reason, same trade. */
        if (!arg_ok && args[i] && args[i]->type.kind != TY_ANY &&
            expected_arg_kind == TY_TYVAR && fn_binding &&
            fn_type.kind == TY_FN &&
            (lang_span_is_dynamic(args[i]->span) ||
             lang_span_is_dynamic(call->span) || e->toplevel_dynamic)) {
            uint32_t fi_w = fn_binding->closure_fn_binding ? i + 1 : i;
            /* A bare tyvar parameter has no entry in arg_full_types (that is
             * for compound args), so fall back to the callee's own FnDef,
             * which keeps the named tyvar -- the same two-step H9 uses. */
            const Type *want_w =
                (fn_type.as.fn.arg_full_types && fi_w < fn_type.as.fn.arity)
                    ? fn_type.as.fn.arg_full_types[fi_w] : NULL;
            if ((!want_w || want_w->kind != TY_TYVAR) &&
                fn_binding->source_fn_def && fn_binding->source_fn_def->params &&
                fi_w < fn_binding->source_fn_def->n_params &&
                fn_binding->source_fn_def->params[fi_w])
                want_w = &fn_binding->source_fn_def->params[fi_w]->type;
            if (want_w && want_w->kind == TY_TYVAR && want_w->as.tyvar_.name) {
                CallTypeBinding sib[16];
                uint8_t n_sib = 0;
                for (uint32_t j = 0; j < n_args; j++) {
                    if (j == i || !args[j] || args[j]->type.kind == TY_ANY) continue;
                    uint32_t fj = fn_binding->closure_fn_binding ? j + 1 : j;
                    const Type *ej =
                        (fn_type.as.fn.arg_full_types && fj < fn_type.as.fn.arity)
                            ? fn_type.as.fn.arg_full_types[fj] : NULL;
                    if (!ej && fn_binding->source_fn_def &&
                        fn_binding->source_fn_def->params &&
                        fj < fn_binding->source_fn_def->n_params &&
                        fn_binding->source_fn_def->params[fj])
                        ej = &fn_binding->source_fn_def->params[fj]->type;
                    if (!ej || !call_type_has_named_tyvar(ej)) continue;
                    (void)call_collect_type_bindings(ej, args[j]->type, sib, &n_sib);
                }
                uint8_t bi_w;
                if (call_find_type_binding(sib, n_sib, want_w->as.tyvar_.name,
                                           &bi_w) &&
                    sib[bi_w].type.kind == TY_ANY) {
                    Expr *widened = elab_coerce_to_any(e, args[i]);
                    if (widened) {
                        args[i] = widened;
                        arg_ok = true;
                        /* Record `V := any` for this call the way the unbox
                         * side does, so the result type follows the argument
                         * rather than collapsing the tyvar to a carrier. */
                        (void)call_collect_type_bindings(want_w, args[i]->type,
                                                         type_bindings,
                                                         &n_type_bindings);
                    }
                }
            }
        }
        /* saffron-open-generic-result-not-grounded: the BORROW twin of the
         * widen seam above.  `map-assoc` / `map-get` check the key against the
         * map through `(tur-map-kcheck m (& k))`, whose `(& K)` binds K from the
         * map -- `any` for every map a dynamic file builds -- and then met the
         * key's own `&cstr`: `function 'tur-map-kcheck' arg 2: expected &?, got
         * &cstr`, reported inside stdlib/map.tur for `(map-assoc (map-new) "k"
         * 42)`.  By value the widen seam boxes such a key; a borrow cannot be
         * widened in place, so borrow a widened copy instead -- `(& (:: k
         * any))`, the address of a fresh box.  Only an immutable borrow (a
         * callee cannot write through it), only when what K is bound to is
         * exactly `any`, and only in a dynamic file: a typed `(Map Sym int)`
         * keeps rejecting a string key (errors/saffron-typed-map-rejects-
         * wrong-key). */
        if (!arg_ok && args[i] && args[i]->kind == EX_BORROW_IMMUT &&
            args[i]->type.kind == TY_REF_IMMUT &&
            args[i]->type.as.ref_borrow.target != TY_ANY &&
            args[i]->as.borrow_immut_.expr &&
            fn_binding && fn_type.kind == TY_FN &&
            (lang_span_is_dynamic(args[i]->span) ||
             lang_span_is_dynamic(call->span) || e->toplevel_dynamic)) {
            uint32_t fb = fn_binding->closure_fn_binding ? i + 1 : i;
            const Type *pb = (fn_type.as.fn.arg_full_types && fb < fn_type.as.fn.arity)
                ? fn_type.as.fn.arg_full_types[fb] : NULL;
            if (!pb && fn_binding->source_fn_def && fn_binding->source_fn_def->params &&
                fb < fn_binding->source_fn_def->n_params &&
                fn_binding->source_fn_def->params[fb])
                pb = &fn_binding->source_fn_def->params[fb]->type;
            uint8_t bix = 0;
            if (pb && pb->kind == TY_REF_IMMUT && pb->as.ref_borrow.target_tyvar &&
                call_find_type_binding(type_bindings, n_type_bindings,
                                       pb->as.ref_borrow.target_tyvar, &bix) &&
                type_bindings[bix].type.kind == TY_ANY) {
                Expr *boxed = elab_coerce_to_any(e, args[i]->as.borrow_immut_.expr);
                if (boxed) {
                    Expr *nb = expr_new(e->arena, EX_BORROW_IMMUT,
                                        type_ref_immut(TY_ANY), args[i]->span);
                    nb->as.borrow_immut_.expr = boxed;
                    args[i] = nb;
                    arg_ok = true;
                }
            }
        }
        if (!arg_ok) {
            /* Phase 8: Enhanced type mismatch with error code */
            /* IT1: Use union-specific error code when union type is involved */
            DiagCode err_code = TUR_E0001_TYPE_MISMATCH;
            if (expected_arg_kind == TY_UNION ||
                args[i]->type.kind == TY_UNION) {
                err_code = TUR_E0300_UNION_TYPE_MISMATCH;
            }
            /* Compute expected type for the diagnostic.
             * For compound types (union, intersection, app, handler) that store
             * their full type in arg_full_types, look it up there so the name
             * includes member/row types. */
            Type expected_ty = type_from_kind(expected_arg_kind);
            /* saffron-open-generic-result-not-grounded (its second defect): a
             * borrowed generic parameter `(& K)` printed as `&?` -- the kind
             * alone has no target -- so `tur-map-kcheck`'s refusal of a string
             * key into a Sym-keyed map read "expected &?, got &cstr".  Print
             * what K is bound to at this call. */
            if ((expected_arg_kind == TY_REF_IMMUT || expected_arg_kind == TY_REF_MUT) &&
                fn_binding && fn_type.kind == TY_FN) {
                uint32_t fr = fn_binding->closure_fn_binding ? i + 1 : i;
                const Type *pr = (fn_type.as.fn.arg_full_types && fr < fn_type.as.fn.arity)
                    ? fn_type.as.fn.arg_full_types[fr] : NULL;
                if (!pr && fn_binding->source_fn_def && fn_binding->source_fn_def->params &&
                    fr < fn_binding->source_fn_def->n_params &&
                    fn_binding->source_fn_def->params[fr])
                    pr = &fn_binding->source_fn_def->params[fr]->type;
                uint8_t rix = 0;
                if (pr && (pr->kind == TY_REF_IMMUT || pr->kind == TY_REF_MUT) &&
                    pr->as.ref_borrow.target_tyvar &&
                    call_find_type_binding(type_bindings, n_type_bindings,
                                           pr->as.ref_borrow.target_tyvar, &rix) &&
                    type_bindings[rix].type.kind != TY_TYVAR &&
                    type_bindings[rix].type.kind != TY_UNKNOWN)
                {
                    expected_ty = pr->kind == TY_REF_MUT
                        ? type_ref_mut(type_bindings[rix].type.kind)
                        : type_ref_immut(type_bindings[rix].type.kind);
                    if (borrow_target_needs_full(type_bindings[rix].type.kind)) {
                        Type *bt = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *bt = type_bindings[rix].type;
                        expected_ty.as.ref_borrow.target_full = bt;
                    }
                }
            }
            if ((expected_arg_kind == TY_UNION || expected_arg_kind == TY_INTERSECTION ||
                 expected_arg_kind == TY_APP || expected_arg_kind == TY_HANDLER ||
                 expected_arg_kind == TY_STRUCT || expected_arg_kind == TY_ADT ||
                 /* session-type-eq-ignores-the-protocol: a kind-derived session
                  * prints as `Session[?]`, which tells the reader nothing about
                  * the protocol they got wrong. The declared protocol is the
                  * whole content of the diagnostic, so take it from
                  * arg_full_types. */
                 expected_arg_kind == TY_SESSION || expected_arg_kind == TY_ROLE ||
                 expected_arg_kind == TY_FN) &&
                fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
                uint32_t fn_arg_idx4 = fn_binding->closure_fn_binding ? i + 1 : i;
                Type *ct = (fn_arg_idx4 < fn_type.as.fn.arity)
                    ? fn_type.as.fn.arg_full_types[fn_arg_idx4] : NULL;
                if (ct) expected_ty = *ct;
            } else if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
                /* self-typed-heap-parametric-field-unsupported: a parameter
                 * whose KIND is the int64 carrier but whose full type names an
                 * application or an ADT -- a `:heap` app field in a ctor
                 * signature, `(next (Node A))` -- printed as `expected int,
                 * got int` against an int argument: the two sides that failed
                 * type_eq were spelled identically.  Prefer the full type. */
                uint32_t fn_arg_idx5 = fn_binding->closure_fn_binding ? i + 1 : i;
                Type *ct = (fn_arg_idx5 < fn_type.as.fn.arity)
                    ? fn_type.as.fn.arg_full_types[fn_arg_idx5] : NULL;
                if (ct && (ct->kind == TY_APP || ct->kind == TY_ADT))
                    expected_ty = *ct;
                /* borrowed-aggregate-key-skips-the-key-check: a concrete
                 * `(& Pt)` prints its target, not `&adt`. */
                if (ct && ct->kind == expected_arg_kind &&
                    (ct->kind == TY_REF_IMMUT || ct->kind == TY_REF_MUT) &&
                    ct->as.ref_borrow.target_full && !ct->as.ref_borrow.target_tyvar &&
                    !call_type_has_named_tyvar(ct))
                    expected_ty = *ct;
            }
            /* parametric-stdlib-diagnostics-print-tyvar-internals: a generic
             * parameter printed as its bare kind -- `(vec-push! v 7.25)` on a
             * `(Vec int)` read "expected tyvar, got float".  By this argument
             * the variable is usually bound (`A := int`, from `v`), and the
             * binding is what the reader needs.  Take the declared type and
             * substitute this call's bindings into it; a still-unbound
             * variable prints under its own name. */
            if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
                uint32_t fti = fn_binding->closure_fn_binding ? i + 1 : i;
                const Type *ft = (fti < fn_type.as.fn.arity)
                    ? fn_type.as.fn.arg_full_types[fti] : NULL;
                if (expected_ty.kind == TY_TYVAR && ft && ft->kind == TY_TYVAR)
                    expected_ty = *ft;
            }
            if (n_type_bindings > 0 && call_type_has_named_tyvar(&expected_ty))
                expected_ty = call_instantiate_type(e, &expected_ty, type_bindings,
                                                    n_type_bindings);
            /* PH2.1: Build the type names into owned local buffers via
             * type_print rather than type_name. type_name returns a strdup-ed
             * heap string for composite kinds (handler, union, fn, ...) that no
             * caller frees -- a real LeakSanitizer-visible leak on every
             * composite-type diagnostic. type_print writes into a Buf we own and
             * free here, so this error path is leak-clean. */
            Buf expected_buf; buf_init(&expected_buf);
            type_print(&expected_buf, expected_ty);
            buf_putc(&expected_buf, '\0');
            Buf actual_buf; buf_init(&actual_buf);
            type_print(&actual_buf, args[i]->type);
            buf_putc(&actual_buf, '\0');
            diag_emit_with_code(DIAG_ERROR, args[i]->span, err_code,
                                "function '%s' arg %u: expected %s, got %s",
                                fn_binding->name->name, i + 1,
                                expected_buf.data,
                                actual_buf.data);
            buf_free(&expected_buf);
            buf_free(&actual_buf);
            /* List-macro tuple hint: a (list ...) literal now routes through
             * the homogeneity check tur-list-homog__ (params share one type
             * variable A), so a mixed-type literal fails there -- and that is
             * the one genuinely-heterogeneous case the tupleN hint is for. The
             * hand-written int-carrier tcons chain still fails on arg 1, so keep
             * keying on it too. Suggest tupleN for heterogeneous fixed-arity
             * needs. */
            if ((strcmp(fn_binding->name->name, "tcons") == 0 && i == 0) ||
                strcmp(fn_binding->name->name, "tur-list-homog__") == 0) {
                diag_emit(DIAG_HELP, args[i]->span,
                          "for heterogeneous fixed-arity collections, "
                          "consider tuple2, tuple3, tuple4, or tuple5 "
                          "instead of (list ...)");
            }
            return NULL;
        }

        if (fn_type.kind == TY_FN) {
            uint32_t fn_arg_idx_cast = fn_binding->closure_fn_binding ? i + 1 : i;
            Type *expected_full = (fn_type.as.fn.arg_full_types &&
                                   fn_arg_idx_cast < fn_type.as.fn.arity)
                ? fn_type.as.fn.arg_full_types[fn_arg_idx_cast] : NULL;
            if ((expected_arg_kind == TY_TYVAR ||
                 (expected_full && expected_full->kind == TY_TYVAR)) &&
                args[i]->type.kind != TY_INT) {
                OwnCarry carry = own_carry_for_arg(
                    fn_binding->name ? fn_binding->name->name : NULL, fn_arg_idx_cast);
                if (carry == OWN_CARRY_RETAIN && own_arg_mints_reference(args[i]))
                    carry = OWN_CARRY_BORROW;
                args[i] = call_wrap_reinterpret_owning(
                    e, args[i], TY_INT, args[i]->span, carry);
                /* The owning-carrier rules can reject (diagnostic already
                 * emitted).  The rest of this loop dereferences args[i]
                 * unconditionally, so bail rather than segfault on the way to
                 * reporting an error we have already reported. */
                if (!args[i]) return NULL;
            }
        }

        /* Phase HRT1/HRT4: wrap rank-2 args with EX_POLY_WRAP + create wrapper thunk.
         * Phase HRT4: also handles TY_PTR_VOID is_poly_fn bindings (pass-through). */
        if (is_rank2_param && (args[i]->type.kind == TY_FN ||
                                args[i]->type.kind == TY_FORALL ||
                                args[i]->type.kind == TY_EXISTS ||
                                args[i]->type.kind == TY_PTR_VOID)) {
            Binding *inner_fn_b = poly_arg_fn_binding(args[i]);
            if (!inner_fn_b) {
                diag_emit(DIAG_ERROR, args[i]->span,
                          "rank-2 argument must be a named function (capturing closures not yet supported)");
                return NULL;
            }
            /* Slice 3 (constrained-hkt-forall): gate + validate a higher-kinded
             * rank-2 parameter.  When the forall quantifies an `f :: * -> *`
             * used as `(f a)` in a body parameter, the passed function's
             * corresponding parameter must be a type application whose base
             * constructor kind matches f's kind. */
            if (rank2_forall_ty && forall_has_higher_kinded_var(rank2_forall_ty)) {
                const Type *fbody = rank2_forall_ty->as.forall_.body;
                if (fbody && fbody->kind == TY_FN && fbody->as.fn.arg_full_types &&
                    inner_fn_b->type.kind == TY_FN &&
                    inner_fn_b->type.as.fn.arg_full_types) {
                    uint8_t env_off = inner_fn_b->closure_fn_binding ? 1 : 0;
                    for (uint32_t bp = 0; bp < fbody->as.fn.arity; bp++) {
                        Kind fk;
                        if (!hrt_body_param_hk_var_kind(rank2_forall_ty,
                                fbody->as.fn.arg_full_types[bp], &fk))
                            continue;
                        uint8_t ap = bp + env_off;
                        if (ap >= inner_fn_b->type.as.fn.arity) continue;
                        Type *act = inner_fn_b->type.as.fn.arg_full_types[ap];
                        if (!act) continue;
                        if (!hrt_validate_hk_actual(e, fk, *act, args[i]->span,
                                                    "rank-2 argument"))
                            return NULL;
                    }
                }
            }
            Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, args[i]->span);
            wrap->as.poly_wrap_.inner = args[i];
            wrap->as.poly_wrap_.boxes_aggregate = true;  /* Slice 3: forall carrier */
            if (inner_fn_b->is_poly_fn) {
                /* HRT4: pass-through — binding is already a tur_poly_fn_t, no wrapper needed. */
                wrap->as.poly_wrap_.wrapper_binding = NULL;
            } else if (!inner_fn_b->is_global) {
                /* local-fn-value-into-rank2-slot-gets-a-by-name-wrapper: a
                 * fn-typed PARAMETER or `let`-bound fn value reaching a rank-2
                 * slot (every `fmap`/`bind` over a by-value container, e.g.
                 * `(Option any)`) cannot go through make_poly_wrapper: it
                 * synthesises a FILE-SCOPE `__poly_N` whose body calls the
                 * binding by name, which is meaningless for a local
                 * (`'f' undeclared`).  The value in the local already IS the
                 * representation the carrier reads -- a fat `{thunk, env}` box
                 * for a parameter or capturing closure, a bare code pointer for
                 * a let-bound non-capturing lambda -- so pack it inline through
                 * the is_closure pass-through, exactly as the `:fn`-carrier
                 * path below does for the same shapes.  A CONSTRAINED forall
                 * is the one case the pass-through cannot serve: its carrier
                 * ABI leads with one dictionary per constraint, and only a
                 * generated wrapper can absorb those slots. */
                if (rank2_forall_ty && rank2_forall_ty->as.forall_.n_constraints > 0) {
                    diag_emit(DIAG_ERROR, args[i]->span,
                              "cannot pass the local function value '%s' to a "
                              "rank-2 parameter whose forall type carries a "
                              "typeclass constraint: the constrained carrier "
                              "leads with a dictionary per constraint, which only "
                              "a named top-level function can absorb -- pass a "
                              "`defn` instead",
                              inner_fn_b->name ? inner_fn_b->name->name : "?");
                    return NULL;
                }
                wrap->as.poly_wrap_.wrapper_binding = NULL;
                wrap->as.poly_wrap_.is_closure = true;
            } else {
                bool forall_constrained = rank2_forall_ty &&
                    rank2_forall_ty->as.forall_.n_constraints > 0;
                uint8_t nc = rank2_forall_ty
                    ? rank2_forall_ty->as.forall_.n_constraints : 0;
                bool inner_poly_constrained = inner_fn_b->fn_constraints &&
                    inner_fn_b->fn_constraints->n_constraints > 0;
                if (forall_constrained) {
                    /* forall-dict-pass GRADUATED 2026-07-06 (MB1 of
                     * constrained-hkt-forall-mode-b-plan): the carrier ABI of a
                     * constrained rank-2 forall carries one dictionary per
                     * constraint (leading args, resolved at each invocation),
                     * always -- the former --enable=forall-dict-pass gate is
                     * gone.  A *polymorphic* constrained inner is wrapped as a
                     * dict-clone that dispatches its class method through that
                     * dict; a *monomorphic* inner accepts and ignores the dict
                     * slots. */
                    if (inner_poly_constrained) {
                        /* forall-dict-pass-multi-constraint-hkt-plan (Task 1.3):
                         * N>=1 constraints are supported -- the clone carries one
                         * dict param per constraint (make_dict_clone) and each
                         * class-method call dispatches through the dict for its
                         * own class.  The only residual limits are the shared
                         * MAX_FN_CONSTRAINTS / MAX_FN_ARITY budget (the forall and
                         * the inner fn must agree on the constraint count so the
                         * leading dict slots line up positionally). */
                        uint8_t inner_nc =
                            inner_fn_b->fn_constraints->n_constraints;
                        if (nc != inner_nc) {
                            diag_emit(DIAG_ERROR, args[i]->span,
                                "forall-dict-pass: constraint count mismatch "
                                "(%u on the forall, %u on '%s') -- the leading "
                                "dictionary slots cannot be aligned", (unsigned)nc,
                                (unsigned)inner_nc,
                                inner_fn_b->name ? inner_fn_b->name->name : "?");
                            return NULL;
                        }
                        if (inner_nc > MAX_FN_CONSTRAINTS) {
                            diag_emit(DIAG_ERROR, args[i]->span,
                                "forall-dict-pass: '%s' has %u typeclass "
                                "constraints, over the limit of %u",
                                inner_fn_b->name ? inner_fn_b->name->name : "?",
                                (unsigned)inner_nc, (unsigned)MAX_FN_CONSTRAINTS);
                            return NULL;
                        }
                        /* make_dict_clone converts every SIMPLE nested mapper
                         * that dispatches a constraint method into a
                         * dict-capturing closure (Phase 2), and itself rejects
                         * any residual un-lowerable nested dispatch with
                         * TUR-E0311 (so both this site and the Gap B composition
                         * site are covered).  A NULL return here is therefore
                         * either that E0311 (already emitted) or a benign
                         * build-failure -- only emit the generic diagnostic when
                         * make_dict_clone did NOT already emit one. */
                        bool had_err_before = diag_had_error();
                        Binding *clone = make_dict_clone(e, inner_fn_b, args[i]->span);
                        if (!clone) {
                            if (had_err_before || !diag_had_error())
                                diag_emit(DIAG_ERROR, args[i]->span,
                                    "forall-dict-pass: could not build a "
                                    "dict-clone for '%s' (its definition must be "
                                    "a plain defn)",
                                    inner_fn_b->name ? inner_fn_b->name->name : "?");
                            return NULL;
                        }
                        Binding *wrapper_b = make_poly_wrapper_ex(
                            e, clone, clone->type.as.fn.arity, 0, args[i]->span, false);
                        if (!wrapper_b) return NULL;
                        wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
                        /* turi-dict-passing-plan: let the interpreter target
                         * the clone directly (it cannot execute the emit-side
                         * __poly wrapper). */
                        wrap->as.poly_wrap_.dict_clone_binding = clone;
                    } else {
                        uint32_t inner_arity = (inner_fn_b->type.kind == TY_FN)
                            ? inner_fn_b->type.as.fn.arity : 1;
                        if (inner_fn_b->closure_fn_binding) inner_arity--;
                        Binding *wrapper_b = make_poly_wrapper_ex(
                            e, inner_fn_b, inner_arity, nc, args[i]->span, false);
                        if (!wrapper_b) return NULL;
                        wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
                    }
                } else if (inner_poly_constrained) {
                    /* Soundness gate: the rank-2 parameter's forall type declares
                     * NO constraint, so there is no dictionary slot to carry the
                     * inner fn's own class obligation.  Its body would monomorphize
                     * to the single carrier representative and miscompile for any
                     * other type -- e.g. `(f true)` runs `Show int` on a bool.
                     * Declare the constraint on the forall parameter type (so the
                     * dict is threaded through the carrier) or pass a monomorphic
                     * function. */
                    diag_emit(DIAG_ERROR, args[i]->span,
                              "cannot pass the polymorphic constrained function "
                              "'%s' as a rank-2 argument: its body dispatches a "
                              "typeclass method on its own type variable, but the "
                              "rank-2 parameter's forall type declares no matching "
                              "constraint to carry the runtime dictionary. Add the "
                              "constraint to the forall parameter type (e.g. "
                              "(forall [a] [(Show a)] (-> a cstr))) or pass a "
                              "monomorphic function (TUR-E0308)",
                              inner_fn_b->name ? inner_fn_b->name->name : "?");
                    return NULL;
                } else {
                    uint32_t inner_arity = (inner_fn_b->type.kind == TY_FN)
                        ? inner_fn_b->type.as.fn.arity : 1;
                    /* Closures have an env param counted in arity — subtract it */
                    if (inner_fn_b->closure_fn_binding) inner_arity--;
                    Binding *wrapper_b = make_poly_wrapper(e, inner_fn_b, inner_arity, args[i]->span, false);
                    if (!wrapper_b) return NULL;
                    wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
                }
            }
            args[i] = wrap;
        }

        /* Phase CCL (fn-first-class-application): a hand-written `:fn` parameter
         * (mono poly-closure carrier, arg_poly_fn) receives a tur_poly_fn_t.  Box
         * the function/lambda/closure argument into the carrier via EX_POLY_WRAP,
         * mirroring the typeclass-method dispatch path:
         *   - a named function / non-capturing lambda -> a generated poly wrapper;
         *   - a capturing closure (TY_PTR_VOID / boxed TY_FN) -> packed inline;
         *   - another `:fn` value (already a carrier) -> passed through.
         * A raw :ptr<void> that is not a closure is left alone here; the regular
         * kind check rejects it (a raw pointer is not callable, by design). */
        if (!is_rank2_param && fn_type.kind == TY_FN) {
            uint32_t fn_arg_idx_pf = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_arg_idx_pf < fn_type.as.fn.arity &&
                FN_ARG_FLAG(fn_type.as.fn, fn_arg_idx_pf, FA_POLY_FN)) {
                Binding *inner_fn_b = poly_arg_fn_binding(args[i]);
                /* F5: a *typed* `:fn` carrier (the param's full type is a concrete
                 * TY_FN signature) stores a natively typed thunk -- make_poly_wrapper
                 * retypes float args, and the closure pass-through stores the
                 * closure's own concrete thunk -- and the call site casts fn.fn to
                 * the concrete R(*)(void*, A...).  So a float-class arg/result
                 * round-trips and the guard below must NOT fire.  Only the *bare*
                 * `:fn` carrier (full type TY_PTR_VOID, int64 default signature)
                 * cannot carry floats. */
                const Type *param_full = (fn_type.as.fn.arg_full_types &&
                                          fn_arg_idx_pf < fn_type.as.fn.arity)
                    ? fn_type.as.fn.arg_full_types[fn_arg_idx_pf] : NULL;
                bool param_typed_carrier = param_full && param_full->kind == TY_FN;
                /* Phase CCL: the bare `:fn` carrier is the int64 register class.  A
                 * function whose signature has a float-class argument or result
                 * cannot round-trip through it without a register-class miscompile
                 * (xmm vs gp); reject the coercion rather than miscompile.  See
                 * docs/archive/history/fn-first-class-float-carrier-gap.md. */
                if (!param_typed_carrier && inner_fn_b && inner_fn_b->type.kind == TY_FN) {
                    const Type *ft = &inner_fn_b->type;
                    bool has_float = (kind_is_non_int_register_class(ft->as.fn.result_kind) ||
                                      ft->as.fn.result_kind == TY_FLOAT32 ||
                                      ft->as.fn.result_kind == TY_FLOAT64);
                    for (uint8_t k = 0; !has_float && k < ft->as.fn.arity; k++) {
                        TypeKind ak = ft->as.fn.arg_kinds[k];
                        if (kind_is_non_int_register_class(ak) ||
                            ak == TY_FLOAT32 || ak == TY_FLOAT64) has_float = true;
                    }
                    if (has_float) {
                        diag_emit(DIAG_ERROR, args[i]->span,
                                  "cannot pass a function with a floating-point "
                                  "argument or result as a `:fn` value: the "
                                  "first-class `:fn` carrier is the int64 register "
                                  "class, so a float would be a silent register-class "
                                  "miscompile; use a typed function parameter "
                                  "(e.g. `g : (fn [float] : float)`) instead");
                        return NULL;
                    }
                }
                if (inner_fn_b) {
                    Expr *orig = args[i];
                    Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, orig->span);
                    wrap->as.poly_wrap_.inner = orig;
                    wrap->as.poly_wrap_.is_closure = false;
                    if (inner_fn_b->is_poly_fn) {
                        /* Already a tur_poly_fn_t -- pass through. */
                        wrap->as.poly_wrap_.wrapper_binding = NULL;
                    } else if (!inner_fn_b->is_global) {
                        /* CRU + typed-fn-param forwarding: any LOCAL fn binding --
                         * a capturing closure VALUE (`(make-add 10)`) OR a plain
                         * typed fn PARAMETER (`g : (fn [a] b)` forwarded into a
                         * `:fn` helper, the M7 Functor-instance shape) -- cannot go
                         * through make_poly_wrapper: it emits a file-scope wrapper
                         * `__poly_N` that statically references the local (`g` /
                         * `add10_904`), which is out of scope at file scope and
                         * fails to compile (`'g' undeclared`).  Pack the runtime
                         * value into the carrier inline instead -- the is_closure
                         * path reads the thunk from the box's slot 0 at runtime, so
                         * both a capturing closure and a passed-in fn round-trip. */
                        wrap->as.poly_wrap_.wrapper_binding = NULL;
                        wrap->as.poly_wrap_.is_closure = true;
                    } else {
                        uint32_t inner_arity = (inner_fn_b->type.kind == TY_FN)
                            ? inner_fn_b->type.as.fn.arity : 1;
                        if (inner_fn_b->closure_fn_binding) inner_arity--;
                        Binding *wrapper_b = make_poly_wrapper(e, inner_fn_b, inner_arity,
                                                               args[i]->span, param_typed_carrier);
                        if (!wrapper_b) return NULL;
                        wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
                    }
                    args[i] = wrap;
                } else if (args[i]->type.kind == TY_PTR_VOID ||
                           (args[i]->type.kind == TY_FN && args[i]->type.as.fn.boxed) ||
                           (args[i]->kind == EX_ANY_CAST &&
                            args[i]->type.kind == TY_FN)) {
                    /* A capturing closure value -- pack it into the carrier.
                     *
                     * saffron-dynamic-surface-pass H7: the Saffron SEAM's
                     * checked unbox (`elab_any_unbox_to`, an EX_ANY_CAST) is
                     * the third shape that belongs here, and it reached
                     * neither of the first two -- it has no fn binding to find
                     * and its TY_FN type is not `boxed` -- so nothing wrapped
                     * it and the raw payload word was handed to a
                     * `tur_poly_fn_t` parameter: `incompatible type for
                     * argument 1 of 'tfn'`, a whole TU cc rejects.
                     *
                     * `is_closure` is the RIGHT path for it rather than a
                     * near-miss: `elab_coerce_to_any` normalises a function
                     * payload to the fat `{ thunk, env }` representation
                     * before boxing it, so an `any` that holds a function
                     * always holds a fat box -- and the is_closure emit reads
                     * the thunk from slot 0 at run time with the box itself as
                     * the env, which is exactly the conversion the D8 dynamic
                     * witness already performs by hand. That it goes through
                     * the box rather than a static wrapper is why this works
                     * for a CAPTURING closure too, not only the captureless
                     * one the filed fix direction proposed to special-case. */
                    Expr *orig = args[i];
                    Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, orig->span);
                    wrap->as.poly_wrap_.inner = orig;
                    wrap->as.poly_wrap_.wrapper_binding = NULL;
                    wrap->as.poly_wrap_.is_closure = true;
                    args[i] = wrap;
                }
            }
        }

        /* A#1: ^fat parameter -- auto-shim a bare (non-capturing) fn into a fat
         * closure so a fat-call consumer (reactor cb, free-bind kont, ...) reads a
         * valid { thunk, env } layout instead of a bare function pointer.  A
         * capturing closure (TY_PTR_VOID) is already fat and passes through; nil is
         * a null callback.  Any other argument kind to a ^fat parameter is the
         * diagnostic half of A#1 -- a typed error instead of a runtime segfault.
         * EX_FN_TO_FAT carries TY_PTR_VOID, mirroring EX_CLOSURE, so it reuses the
         * same arg-emission casts that already feed closures to :fn/:int/:ptr. */
        if (!is_rank2_param && fn_type.kind == TY_FN) {
            uint32_t fn_arg_idx_fat = fn_binding->closure_fn_binding ? i + 1 : i;
            bool slot_fat_decl = fn_arg_idx_fat < fn_type.as.fn.arity &&
                FN_ARG_FLAG(fn_type.as.fn, fn_arg_idx_fat, FA_FAT);
            /* fn-value-fat-normalization stage 1: a NOMINAL thin TY_FN param
             * slot gets the same treatment as a ^fat one -- every fn value
             * flowing in is normalized to a fat handle, because the callee's
             * invoke now dispatches fat (emit_expr.c ER2, keyed on the SAME
             * fn_param_type_is_fat_normalized predicate).  Carrier-eligible
             * params never appear here (their arg_kind is TY_PTR_VOID by
             * elab-defn time); cfnptr/variadic/arity>5 stay thin by the
             * predicate.  The diagnostic else-branch below stays ^fat-only:
             * shapes the checker already accepts at a nominal param must not
             * become new errors. */
            bool slot_nominal = false;
            if (!slot_fat_decl && fn_arg_idx_fat < fn_type.as.fn.arity &&
                fn_type.as.fn.arg_kinds[fn_arg_idx_fat] == TY_FN &&
                fn_type.as.fn.arg_full_types) {
                const Type *aft_nom = fn_type.as.fn.arg_full_types[fn_arg_idx_fat];
                slot_nominal = aft_nom && fn_param_type_is_fat_normalized(aft_nom);
                /* poly-result-hof-capturing-closure-sigbus, residual arm: the
                 * effect-row exclusion was lifted 2026-08-16 (effect-annotated
                 * fn params are fat-normalized; slot_nominal is true for them
                 * and this diagnostic no longer fires there).  What remains
                 * THIN is the predicate's other rejects -- cfnptr, variadic,
                 * arity>5 -- and a capturing closure into an EFFECT-ANNOTATED
                 * such param still has nowhere to carry its environment: the
                 * callee would jump into the env box as code at run time
                 * (SIGBUS/SIGSEGV).  Keep the call-site error for exactly
                 * those shapes, placed before the `^borrow` hoist hides the
                 * closure literal behind a temp. */
                if (!slot_nominal && aft_nom && aft_nom->kind == TY_FN &&
                    aft_nom->as.fn.effect_row && !aft_nom->as.fn.cfnptr) {
                    Expr *cl = args[i];
                    while (cl && cl->kind == EX_ASCRIBE)
                        cl = cl->as.ascribe_.inner;
                    if (cl && cl->kind == EX_CLOSURE &&
                        cl->as.closure_.closure &&
                        cl->as.closure_.closure->n_captures > 0) {
                        diag_emit_with_code(DIAG_ERROR, cl->span,
                            TUR_E0007_CAPTURE_ERROR,
                            "capturing closure passed to effectful fn "
                            "parameter %u of '%s': this parameter shape "
                            "(cfnptr, variadic, or arity above 5) keeps the "
                            "thin calling convention, which cannot carry a "
                            "closure environment -- this call would crash at "
                            "run time. Reduce the signature to 5 or fewer "
                            "non-variadic parameters, or pass the captured "
                            "state as explicit arguments",
                            (unsigned)(i + 1),
                            fn_binding->name ? fn_binding->name->name
                                             : "<fn>");
                    }
                }
            }
            if (slot_fat_decl || slot_nominal) {
                /* erased-closure-param-over-niche-vec-slot-reads-box: the
                 * comparator is fed raw container words.  Mark a lambda's
                 * parameters so an ascription back to the element type
                 * reinterprets the word instead of unboxing a box that is not
                 * there.  A name cannot carry the mark; when the element is a
                 * niche option -- the one element form whose word is not a
                 * box -- that shape has no decidable convention, so say so
                 * rather than compile a silent wrong answer.  This is the read
                 * side of the store side's TUR-E0714.
                 *
                 * The refusal reaches `vec-eq?` only, because it needs the
                 * element type and only a `(Vec A)` receiver carries one:
                 * `list-eq?` declares `l1 : int`, a bare cons carrier with the
                 * element erased at the call, so a named erased comparator over
                 * a list stays undiagnosable.  Marking still covers every list
                 * comparator written inline. */
                if (call_comparator_gets_carrier_box(fn_binding, i))
                    mark_comparator_carrier_box_params(args[i]);
                if (call_comparator_gets_slot_words(fn_binding, i) &&
                    !mark_comparator_slot_word_params(e, args[i]) &&
                    n_args > 0 && args[i]->type.kind == TY_FN &&
                    args[i]->type.as.fn.arity > 0 &&
                    args[i]->type.as.fn.arg_kinds &&
                    /* Only an ERASED parameter is undecidable.  A comparator
                     * whose parameters are declared as the element type already
                     * bridges at the closure boundary and is correct. */
                    args[i]->type.as.fn.arg_kinds[0] == (uint8_t)TY_INT) {
                    Type et = comparator_vec_elem_type(args[0]);
                    if (adt_app_is_niche_option(et)) {
                        /* type_name spells a TY_APP in the internal
                         * `(type-app Option String)` form; a diagnostic the
                         * user has to act on quotes the surface spelling. */
                        char ebuf[256];
                        snprintf(ebuf, sizeof ebuf, "(%s %s)",
                                 et.as.app.fn ? type_name(*et.as.app.fn) : "?",
                                 et.as.app.arg ? type_name(*et.as.app.arg) : "?");
                        const char *ets = ebuf;
                        diag_emit_with_code(DIAG_ERROR, args[i]->span,
                            TUR_E0715_NICHE_ELEMENT_ERASED_COMPARATOR,
                            "the comparator passed to '%s' over a "
                            "'(Vec %s)' is named rather than written inline, so "
                            "its parameters cannot be marked as the raw slot "
                            "words this helper hands them. A niche '%s' rides "
                            "its slot as the payload pointer, not as a carrier "
                            "box, so an erased parameter has no decidable "
                            "convention here. Declare the parameter types -- "
                            "(fn [a : %s b : %s] ...) -- or write the "
                            "comparator inline at the call",
                            fn_binding->name->name, ets, ets, ets, ets);
                        return NULL;
                    }
                }
                /* fat-closure-ascription: an *already-fat* closure value that is
                 * carried as a one-word :int/:ptr<void> (e.g. a list-head result,
                 * or a handler threaded around as :int) and ascribed to a (fn ...)
                 * type via (:: v T) is already in fat { thunk, env } form.  The
                 * ascription is erased at codegen, but it re-types the value to
                 * TY_FN, which would otherwise drive the bare-fn auto-shim below
                 * and wrap the closure-record pointer in a __tur_fatshim adapter
                 * -- so dispatch later calls the record's first 8 bytes as code
                 * and BUS/SEGV-faults.  Strip the erased ascription so the raw
                 * carrier flows to the already-fat pass-through branch (the same
                 * plumbing that threads a computed :int handle untouched). */
                if (args[i]->kind == EX_ASCRIBE && args[i]->as.ascribe_.inner &&
                    args[i]->type.kind == TY_FN) {
                    TypeKind carrier_k = args[i]->as.ascribe_.inner->type.kind;
                    if (carrier_k == TY_INT || carrier_k == TY_PTR_VOID) {
                        args[i] = args[i]->as.ascribe_.inner;
                    }
                }
                /* captureless-closure-not-boxed-at-fat-ptr-void-boundary: the
                 * mirror image of the strip above.  A *captureless* closure
                 * (e.g. the SF returned by a nullary `(invert)`) is codegen'd as
                 * a BARE fn pointer, not a { thunk, env } fat box.  When it is
                 * ascribed to the carrier with `(:: <captureless-fn> :ptr<void>)`
                 * the ascription retypes the value to TY_PTR_VOID, which would
                 * sail through the already-fat pass-through branch below
                 * unshimmed -- the ^fat consumer then reads the code address as
                 * slot 0 of a fat box and fat-calls garbage (segfault).  Strip
                 * the erased ascription so the inner bare (unboxed) TY_FN reaches
                 * the auto-shim branch and gets boxed via EX_FN_TO_FAT, exactly
                 * like an un-ascribed bare fn argument.  A *capturing* closure's
                 * inner is a boxed TY_FN / TY_PTR_VOID and is left untouched
                 * (stripping it would not change the already-fat pass-through). */
                if (args[i]->kind == EX_ASCRIBE && args[i]->as.ascribe_.inner &&
                    args[i]->type.kind == TY_PTR_VOID &&
                    args[i]->as.ascribe_.inner->type.kind == TY_FN &&
                    !args[i]->as.ascribe_.inner->type.as.fn.boxed) {
                    args[i] = args[i]->as.ascribe_.inner;
                }
                /* two-level-sf-closure-return-miscompiles-out-binding: an
                 * already-fat value -- a ^fat parameter (or a let-alias of one),
                 * marked is_fat -- whose declared type is a concrete `(fn ...)`
                 * is ALREADY a { thunk, env } box, carried as the int64_t/ptr
                 * carrier.  The bare-fn auto-shim branch below keys off
                 * `ak == TY_FN && !boxed` and would wrap this box in a SECOND
                 * __tur_fatshim adapter (double-boxing), so the consumer reads
                 * the inner box's first word as code and SEGVs.  Retype it to the
                 * :ptr<void> carrier so it flows through the already-fat
                 * pass-through branch -- identical to a bare `^fat` parameter
                 * (which carries TY_PTR_VOID and works). */
                if (args[i]->kind == EX_VAR && args[i]->as.var.binding &&
                    (args[i]->as.var.binding->is_fat ||
                     /* fn-value-fat-normalization stage 1: a NORMALIZED nominal
                      * param already holds a fat handle -- forwarding it into
                      * another fat/normalized slot must pass through, not
                      * re-shim (the double-box reads the handle as code and
                      * SEGVs -- the s1c forwarding probe). */
                     (args[i]->as.var.binding->is_param &&
                      fn_param_type_is_fat_normalized(&args[i]->as.var.binding->type))) &&
                    args[i]->type.kind == TY_FN &&
                    !args[i]->type.as.fn.boxed) {
                    args[i]->type = TYPE_PTR_VOID;
                }
                TypeKind ak = args[i]->type.kind;
                bool arg_is_poly_fn = (args[i]->kind == EX_VAR &&
                                       args[i]->as.var.binding &&
                                       args[i]->as.var.binding->is_poly_fn);
                /* Shared by the poly-to-fat and bare-fn-to-fat conversions
                 * below: does the callee provably neither retain nor drop
                 * this argument?  See the comment on the bare-fn shim for why
                 * a set nonretain bit implies both. */
                bool sink_is_nonretaining =
                    fn_binding && i < 32 &&
                    ((fn_binding->nonretain_param_mask & (1u << i)) != 0 ||
                     (fn_binding->type.kind == TY_FN &&
                      i < fn_binding->type.as.fn.arity &&
                      fn_binding->type.as.fn.arg_flags &&
                      FN_ARG_FLAG(fn_binding->type.as.fn, i, FA_BORROW)));
                if (arg_is_poly_fn) {
                    /* SC7: a tur_poly_fn_t value (a typeclass-method closure
                     * param, marked is_poly_fn) is a 16-byte {env,fn} struct,
                     * not a single-int64 fat handle.  Box it into a fat-closure
                     * handle so the ^fat consumer's TUR_APPLY can fat-call it --
                     * this is what lets a Functor/Applicative instance hand its
                     * closure argument to a ^fat schema combinator. */
                    /* poly-to-fat-typed-shim-plan: thread the sink's declared
                     * ^fat fn signature (when it is a concrete TY_FN) so the
                     * emitter can select a slot-0 shim of the matching arity
                     * whose typed-thunk-cast ABI matches the invocation the sink
                     * applies.  For an int64/pointer-carrier ^fat param (no
                     * concrete fn type) this stays NULL and the int64
                     * __tur_poly_to_fat1 shim is used.  A binary or higher-arity
                     * sink selects __tur_poly_to_fat<N>: the carrier's slot 1
                     * holds the method's real N-ary thunk (make_poly_wrapper), so
                     * every argument is forwarded.  (See
                     * docs/archive/history/poly-to-fat-drops-args-beyond-first-multiarg-method.md.) */
                    const Type *eft = (fn_type.as.fn.arg_full_types &&
                                       fn_arg_idx_fat < fn_type.as.fn.arity)
                        ? fn_type.as.fn.arg_full_types[fn_arg_idx_fat] : NULL;
                    Expr *conv = expr_new(e->arena, EX_POLY_TO_FAT, TYPE_PTR_VOID,
                                          args[i]->span);
                    conv->as.poly_to_fat_.inner = args[i];
                    conv->as.poly_to_fat_.sink_fn_type =
                        (eft && eft->kind == TY_FN) ? eft : NULL;
                    /* poly-to-fat-box-leaks-per-call: the same ownership fact
                     * the bare-fn shim reads (below) -- a proven non-retaining
                     * ^fat sink cannot keep or drop the box, so it may live on
                     * the caller's stack.  Every carrier-bodied HKT instance
                     * that forwards its method closure to a ^fat helper
                     * (`Functor [(Either E)]`'s fmap -> `either-map`) minted a
                     * 32-byte box per call that nothing freed. */
                    conv->as.poly_to_fat_.stack_ok =
                        slot_fat_decl && sink_is_nonretaining;
                    args[i] = conv;
                } else if (ak == TY_FN && !args[i]->type.as.fn.boxed) {
                    /* A bare (non-capturing) fn reference -- auto-shim it into a
                     * fat box.  A *boxed* TY_FN (CRU B-1: a capturing closure
                     * value) is already a fat { thunk, env... } box, so it falls
                     * through to the pass-through branch below exactly as a
                     * TY_PTR_VOID closure did pre-B-1; shimming it here would
                     * double-box and segfault. */
                    uint32_t inner_arity = args[i]->type.as.fn.arity;
                    if (inner_arity > 5) {
                        diag_emit(DIAG_ERROR, args[i]->span,
                            "fat (^fat) parameter of '%s' cannot shim an arity-%u "
                            "function (auto-shim supports up to 5 arguments)",
                            fn_binding->name->name, (unsigned)inner_arity);
                        return NULL;
                    }
                    Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, TYPE_PTR_VOID,
                                          args[i]->span);
                    shim->as.fn_to_fat_.inner = args[i];
                    /* hkt-generic-forwarded-bind-continuation-segfaults: when
                     * this slot's declared fn type returns an HKT-erased
                     * `(M b)` (a tyvar-headed application), its consumers read
                     * the result as a carrier, so a by-value aggregate result
                     * must be boxed by slot 0's shim. */
                    shim->as.fn_to_fat_.erased_result =
                        sink_fn_result_is_hkt_erased(&fn_type, fn_arg_idx_fat);
                    {
                        const Type *sft = (fn_type.as.fn.arg_full_types &&
                                           fn_arg_idx_fat < fn_type.as.fn.arity)
                            ? fn_type.as.fn.arg_full_types[fn_arg_idx_fat] : NULL;
                        if (sft && sft->kind == TY_FN && !call_type_has_named_tyvar(sft))
                            shim->as.fn_to_fat_.sink_fn_type = sft;
                    }
                    /* A normalized NOMINAL param never drops its argument --
                     * which is precisely why this shim leaked a box per call --
                     * so its box may be the shared file-scope one.
                     *
                     * A `^fat` sink (slot_fat_decl) used to be excluded outright,
                     * on the grounds that such a callee MAY take ownership and
                     * drop, and its droppedness is not visible at the call site.
                     * The first half is true; the second is not, because the
                     * only way to drop is `TUR_CLOSURE_DROP`, which is a C macro
                     * and so reachable only from an inline-C body -- and a body
                     * containing any inline-C has `nonretain_param_mask == 0` by
                     * construction (elab_fns.c zeroes it there, precisely because
                     * a C body can stash a param where no AST walk can see it).
                     *
                     * So a set nonretain bit already implies the callee neither
                     * retains NOR drops this argument, which is exactly the
                     * ownership fact the shared box needs.  It also sidesteps the
                     * -Wfree-nonheap-object problem that ruled the static box out
                     * before: no free is emitted for a callee that cannot drop.
                     *
                     * Without this, every `^fat` call site mallocs a fresh
                     * {shim, orig} box that nothing ever frees -- ~205 bytes per
                     * call, 822 MiB over 4e6 iterations.  See the static_ok
                     * comment in expr.h and
                     * docs/archive/fat-sink-shim-box-leaks-per-call.md. */
                    /* residual-leaks (2026-09-02): a DECLARED `^borrow` on the
                     * sink is the same fact stated by the callee -- the only way
                     * an inline-C body (whose inferred mask is zero by
                     * construction) can say it invokes and never retains.  The
                     * stdlib comparators (`result-eq?`, `vec-eq?`, `map-eq-raw?`
                     * ...) declare it; without this each call minted a 24-byte
                     * shim box nothing freed. */
                    shim->as.fn_to_fat_.static_ok =
                        (slot_nominal && !slot_fat_decl) ||
                        (slot_fat_decl && sink_is_nonretaining);
                    shim->as.fn_to_fat_.stack_ok =
                        slot_fat_decl && sink_is_nonretaining;
                    args[i] = shim;
                } else if (ak == TY_PTR_VOID || (ak == TY_FN && args[i]->type.as.fn.boxed) ||
                           ak == TY_NIL ||
                           (ak == TY_INT && args[i]->kind == EX_INT_LIT &&
                            args[i]->as.i == 0) ||
                           (ak == TY_INT && args[i]->kind != EX_INT_LIT)) {
                    /* hkt-generic-forwarded-bind-continuation-segfaults: a
                     * capturing closure whose result is a by-value aggregate,
                     * headed for a slot whose declared result is an HKT-erased
                     * `(M b)`, is wrapped so the wrapper's shim boxes the
                     * result into the carrier its consumers read. */
                    if (ak == TY_FN && args[i]->type.as.fn.boxed &&
                        args[i]->type.as.fn.result_full_type &&
                        type_is_byvalue_aggregate_result(
                            *args[i]->type.as.fn.result_full_type) &&
                        sink_fn_result_is_hkt_erased(&fn_type, fn_arg_idx_fat)) {
                        Expr *w = expr_new(e->arena, EX_FN_TO_FAT, TYPE_PTR_VOID,
                                           args[i]->span);
                        w->as.fn_to_fat_.inner = args[i];
                        w->as.fn_to_fat_.erased_result = true;
                        w->as.fn_to_fat_.inner_is_fat = true;
                        args[i] = w;
                    }
                    /* fnsan-nil-closure-into-erased-result: a capturing
                     * closure that returns nil, headed for a `(fn [...] A)`
                     * slot.  Its thunk is `void`; the slot's consumers call it
                     * as returning the word.  Wrapped the same way, with a
                     * shim that calls it and answers 0 -- on the stack when the
                     * sink provably neither keeps nor drops it. */
                    else if (ak == TY_FN && args[i]->type.as.fn.boxed &&
                             (args[i]->type.as.fn.result_full_type
                                  ? args[i]->type.as.fn.result_full_type->kind == TY_NIL
                                  : args[i]->type.as.fn.result_kind == TY_NIL) &&
                             sink_fn_result_is_bare_tyvar(&fn_type, fn_arg_idx_fat)) {
                        Expr *w = expr_new(e->arena, EX_FN_TO_FAT, TYPE_PTR_VOID,
                                           args[i]->span);
                        w->as.fn_to_fat_.inner = args[i];
                        w->as.fn_to_fat_.inner_is_fat = true;
                        w->as.fn_to_fat_.nil_result_word = true;
                        /* The wrapper holds only a borrow of the handle, so
                         * the proof that the callee neither keeps nor drops
                         * the argument is all its frame lifetime needs. */
                        w->as.fn_to_fat_.stack_ok = sink_is_nonretaining;
                        args[i] = w;
                    }
                    /* fnsan-fat-closure-at-tyvar-sink: a capturing closure
                     * headed for a slot typed `(fn [A A] bool)` -- `vec-eq?`'s
                     * comparator.  An inline-C body or carrier base calls it
                     * with words at the `A`s; a `(fn [a : float b : float]
                     * ...)` thunk read them from xmm registers that held
                     * nothing (`vec-eq?` answered true for 7.1 vs 3.25).
                     * Marked here; the emitter wraps it in a word adapter only
                     * when the selected callee reads words and the closure's
                     * thunk does not take them.  Only at a sink proven not to
                     * keep it: the wrapper borrows the handle. */
                    else if (((ak == TY_FN && args[i]->type.as.fn.boxed) ||
                              (arg_is_typed_fat_param(args[i]) &&
                               fn_binding && fn_binding->body_is_inline_c)) &&
                             sink_is_nonretaining &&
                             sink_fn_has_tyvar_param(&fn_type, fn_arg_idx_fat)) {
                        Expr *w = expr_new(e->arena, EX_FN_TO_FAT, TYPE_PTR_VOID,
                                           args[i]->span);
                        w->as.fn_to_fat_.inner = args[i];
                        w->as.fn_to_fat_.inner_is_fat = true;
                        w->as.fn_to_fat_.word_params = true;
                        w->as.fn_to_fat_.stack_ok = true;
                        args[i] = w;
                    }
                    /* Pass through unchanged: a fat closure (TY_PTR_VOID), nil, a
                     * null (literal 0) callback, or an already-erased :int
                     * fat-closure handle (a computed value, e.g. a handler that
                     * compose-middleware/compose-middleware-of has already boxed).
                     * The :int-handle case lets a ^fat boundary param sit on the
                     * same plumbing that threads composed handlers as :int without
                     * re-boxing them; a bare non-capturing fn still arrives as
                     * TY_FN at its first boundary and is shimmed above. */
                } else if (slot_fat_decl) {
                    Buf gb; buf_init(&gb);
                    type_print(&gb, args[i]->type);
                    buf_putc(&gb, '\0');
                    diag_emit(DIAG_ERROR, args[i]->span,
                        "argument %u to fat (^fat) parameter of '%s' must be a "
                        "function or closure, got %s",
                        i + 1, fn_binding->name->name, gb.data);
                    buf_free(&gb);
                    return NULL;
                }
                /* slot_nominal with an unrecognized arg kind: leave the
                 * argument untouched -- the checker already accepted this
                 * shape under the thin convention, and stage 1 must not turn
                 * accepted programs into errors.  (If such a value is invoked
                 * fat it was already broken; the fuzzer's known probes track
                 * those shapes.) */
            }
        }

        /* UT1: TUR_E0200 -- reject aliased value passed to ^unique parameter */
        if (fn_type.kind == TY_FN &&
            i < fn_type.as.fn.arity && FN_ARG_FLAG(fn_type.as.fn, i, FA_UNIQUE) &&
            args[i]->kind == EX_VAR) {
            Binding *arg_b = args[i]->as.var.binding;
            if (arg_b->alias_state == AS_ALIASED) {
                if (arg_b->alias_name) {
                    diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                        TUR_E0200_UNIQUE_ALIASED,
                                        "value '%s' is not unique -- aliased by '%s'",
                                        arg_b->name->name, arg_b->alias_name->name);
                } else {
                    diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                        TUR_E0200_UNIQUE_ALIASED,
                                        "value '%s' is not unique -- it has been aliased",
                                        arg_b->name->name);
                }
                return NULL;
            }
        }

        /* UT2: Reject argument passed to ^unique ^mut param when active borrows exist.
         * A ^unique ^mut parameter requires exclusive mutable access; any live borrow
         * (&T or &mut T) on the same binding would violate that guarantee. */
        if (fn_type.kind == TY_FN &&
            i < fn_type.as.fn.arity && FN_ARG_FLAG(fn_type.as.fn, i, FA_UNIQUE_MUT) &&
            args[i]->kind == EX_VAR) {
            Binding *arg_b = args[i]->as.var.binding;
            if (scope_borrow_conflicts(e->scope, arg_b, BK_MUT)) {
                diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                    TUR_E0200_UNIQUE_ALIASED,
                                    "cannot pass '%s' as ^unique ^mut -- active borrow exists",
                                    arg_b->name->name);
                return NULL;
            }
            /* The binding is itself a `^borrow` PARAMETER.
             *
             * scope_borrow_conflicts only sees borrows registered in THIS frame
             * by an explicit `(& x)`, and a `^borrow` parameter registers
             * nothing -- the aliasing happened in the caller.  So a shared
             * reference could be handed straight to an exclusive parameter with
             * no diagnostic at all, and the callee's mutation was observable
             * through the caller's borrow: exactly what ^unique ^mut exists to
             * rule out.  A by-value struct hides it (the callee mutates a copy);
             * a heap-backed carrier like `(Vec int)` shows it.
             *
             * Rejecting the crossing directly, rather than registering ^borrow
             * params as frame-live borrows, is the narrower of the two fixes the
             * report weighs: it says only "a shared reference cannot become an
             * exclusive one", which is true independent of what the frame knows,
             * and it cannot make any other borrow check noisier.
             * See docs/archive/borrow-param-passed-as-unique-mut-undiagnosed.md. */
            if (arg_b->is_borrow) {
                diag_emit_with_code(DIAG_ERROR, args[i]->span,
                                    TUR_E0200_UNIQUE_ALIASED,
                                    "cannot pass '%s' as ^unique ^mut -- it is a "
                                    "^borrow parameter, so the caller still holds "
                                    "it as a shared reference and the callee's "
                                    "mutation would be visible through it",
                                    arg_b->name->name);
                return NULL;
            }
        }

        /* Phase 11: Move tracking - if arg is a CK_MOVE binding reference, poison it.
         * UT2 semantics:
         *  - ^unique ^mut param: exclusive mutable ACCESS (borrow-like); caller keeps
         *    ownership -- do NOT mark arg as moved, regardless of arg's own flags.
         *  - ^unique param (no ^mut): OWNERSHIP TRANSFER; mark arg as moved so it
         *    can't be used again.
         *  - ordinary param with a ^unique ^mut arg binding: also do not consume,
         *    since the binding is a mutable unique cell that may be freely read. */
        if (args[i]->kind == EX_VAR && type_is_move(args[i]->as.var.binding->type)) {
            Binding *arg_b2 = args[i]->as.var.binding;
            bool param_is_unique_mut = fn_type.kind == TY_FN &&
                i < fn_type.as.fn.arity && FN_ARG_FLAG(fn_type.as.fn, i, FA_UNIQUE_MUT);
            bool arg_is_unique_mut = arg_b2->is_unique && arg_b2->is_mut;
            /* LB1: a ^borrow parameter reads its argument without taking
             * ownership, so it must NOT move (poison) the binding -- otherwise a
             * move-typed (e.g. :affine) handle could not be read by a borrowing
             * accessor and then used again (TUR-E0005). This is the move-checker
             * half of the borrow form; the linear/affine usage rollback below
             * handles the -Xlinear / substructural budgets. */
            bool param_is_borrow = false;
            if (fn_type.kind == TY_FN) {
                uint32_t fn_borrow_idx = fn_binding->closure_fn_binding ? i + 1 : i;
                if (fn_borrow_idx < fn_type.as.fn.arity)
                    param_is_borrow = FN_ARG_FLAG(fn_type.as.fn, fn_borrow_idx, FA_BORROW);
            }
            if (!param_is_unique_mut && !arg_is_unique_mut && !param_is_borrow) {
                /* byvalue-recursive-adt-boxes-are-never-freed (Residue 1): a
                 * by-value recursive ADT local passed to a callee whose mask
                 * says it does not retain the parameter is LENT, not moved:
                 * the static answer is the same (poisoned for later uses),
                 * but ownership of the spine stays here, and this scope's
                 * exit frees it.  Same guards as the frame-box rule beside
                 * the `any` widen -- a direct, effect-free, non-closure
                 * callee (the mask is indexed by the callee's own parameter
                 * vector, and a closure's env parameter shifts it by one). */
                bool lend = fn_binding && !fn_binding->closure_fn_binding &&
                            i < 32 && fn_type.kind == TY_FN &&
                            (fn_binding->nonretain_ptr_param_mask & (1u << i)) &&
                            effect_row_is_empty(fn_type.as.fn.effect_row) &&
                            elab_byval_localowned_adt(arg_b2->type) != NULL;
                if (lend)
                    binding_mark_lent(arg_b2, args[i]->span);
                else
                    binding_mark_moved(arg_b2, args[i]->span);
            }
        }

        /* LB1: ^borrow parameter -- the argument is read but NOT consumed.
         * The var-use elaboration above already recorded a consumption on a
         * fresh linear binding (or emitted TUR-E0101 and bailed if the handle
         * was already consumed, e.g. a free-then-borrow ordering).  Reaching
         * here means the borrow is legal, so roll back the consumption: the
         * single-consumption obligation is preserved for a later consuming op
         * (fs/tmpfile-free, mutex-free, ...).  This is the call-site half of
         * the borrow form -- see docs/archive/history/stdlib-linear-handle-borrows.md. */
        if (args[i]->kind == EX_VAR && fn_type.kind == TY_FN) {
            uint32_t fn_borrow_idx = fn_binding->closure_fn_binding ? i + 1 : i;
            if (fn_borrow_idx < fn_type.as.fn.arity &&
                    FN_ARG_FLAG(fn_type.as.fn, fn_borrow_idx, FA_BORROW)) {
                Binding *arg_b3 = args[i]->as.var.binding;
                if (arg_b3->is_linear) {
                    arg_b3->is_linear_consumed = false;
                }
                /* Substructural affine: undo the single use the var-use path
                 * recorded so the borrow does not spend the at-most-once budget. */
                if (arg_b3->is_affine
                        && arg_b3->usage_state == USAGE_USED_ONCE) {
                    arg_b3->usage_state = USAGE_UNUSED;
                }
            }
        }
    }

    /* open-arg-first-binds-call-result-as-int: a return-only generic call
     * argument (`(none)`, `(vec-new)`) checked before the sibling that fixed
     * its variable -- `(get-or (none) 1.5)` -- is grounded now, the way the
     * nullary-generic block grounds one checked after it: the substitution is
     * recorded on the call so emit monomorphizes it, and the grounded type
     * becomes the argument's.  Left open, the call's bindings said `A :=
     * float` (or `int`, from an unannotated lambda's parameters) while the
     * argument still said `(Option A)`, and a CPS caller named a spec that was
     * never emitted. */
    if (open_first && fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types) {
        for (uint32_t i = 0; i < n_args; i++) {
            if (!open_first[i] || !args[i] || !w2_arg_is_free_poly_call(args[i]))
                continue;
            uint32_t pidx = fn_binding->closure_fn_binding ? i + 1 : i;
            if (pidx >= fn_type.as.fn.arity) continue;
            const Type *pf = fn_type.as.fn.arg_full_types[pidx];
            if (!pf || pf->kind != TY_APP) continue;
            Type inst = call_instantiate_type(e, pf, type_bindings, n_type_bindings);
            CallTypeBinding ogscratch[16];
            uint8_t ogn = 0;
            bool grounded = inst.kind == TY_APP && !call_type_has_named_tyvar(&inst);
            if (!grounded ||
                !call_collect_type_bindings(&args[i]->type, inst, ogscratch, &ogn)) {
                if (!open_first_strict[i]) continue;
                /* Accepted before its siblings were in, and still does not
                 * fit: the mismatch the loop would have reported. */
                Buf eb; buf_init(&eb);
                type_print(&eb, inst);
                buf_putc(&eb, '\0');
                Buf ab; buf_init(&ab);
                type_print(&ab, args[i]->type);
                buf_putc(&ab, '\0');
                diag_emit_with_code(DIAG_ERROR, args[i]->span, TUR_E0001_TYPE_MISMATCH,
                                    "function '%s' arg %u: expected %s, got %s",
                                    fn_binding->name->name, i + 1, eb.data, ab.data);
                buf_free(&eb);
                buf_free(&ab);
                return NULL;
            }
            if (ogn > 0 && !args[i]->as.call_.abi_bindings) {
                AbiTypeBinding *saved = (AbiTypeBinding *)arena_alloc(
                    e->arena, ogn * sizeof(AbiTypeBinding));
                for (uint8_t bi = 0; bi < ogn; bi++) saved[bi] = ogscratch[bi];
                args[i]->as.call_.abi_bindings   = saved;
                args[i]->as.call_.n_abi_bindings = ogn;
            }
            args[i]->type = inst;
        }
    }

    if (fn_type.kind == TY_FN &&
        !call_bind_assoc_projections(e, call, fn_binding, &fn_type,
                                     type_bindings, &n_type_bindings))
        return NULL;

    /* S4 (vec-new + vec-push! forward element inference): a generic call whose
     * receiver argument is a *local* let-bound EX_VAR with an under-constrained
     * parameterised type -- e.g. `rs : (Vec A)` from `(vec-new)`, where A was
     * never pinned -- and a sibling argument that fixes the SAME parameter
     * tyvar to a concrete type -- e.g. `(vec-push! rs 10)`, whose `val : A` slot
     * pins A := int -- should resolve the receiver binding's element type.
     * Without this, `rs` keeps `(Vec A)` and a later `(sum rs)` where
     * `sum : (Vec int)` fails to unify.  The fix is scoped narrowly: it only
     * upgrades a local let binding (never a function parameter or global, whose
     * tyvar may be a genuinely abstract type param), only ever replaces an
     * unresolved tyvar with a concrete type (never the reverse), and leaves the
     * current call's own `type_bindings` -- and thus its carrier/relay emission
     * -- untouched.  It composes off the callee's OWN parameter tyvar names, so
     * it does not depend on the receiver's tyvar name matching the pusher's. */
    if (fn_type.kind == TY_FN && fn_type.as.fn.arg_full_types && n_args > 1 &&
        e->macro_expand_depth == 0) {
        /* Map each callee parameter tyvar that a concrete sibling argument
         * fills (param slot is a bare TY_TYVAR) to that concrete type. */
        CallTypeBinding fwd[16];
        uint8_t n_fwd = 0;
        for (uint32_t i = 0; i < n_args; i++) {
            uint32_t pidx = fn_binding->closure_fn_binding ? i + 1 : i;
            if (pidx >= fn_type.as.fn.arity) break;
            const Type *pf = fn_type.as.fn.arg_full_types[pidx];
            if (!pf || pf->kind != TY_TYVAR || !pf->as.tyvar_.name || !args[i]) continue;
            /* The arg-check loop above carrier-wraps a non-int scalar passed at a
             * tyvar slot (EX_REINTERPRET to TY_INT).  Peel that wrap so the
             * forwarded element type is the TRUE pushed type (float/bool/cstr),
             * not the int64 carrier -- otherwise `(Vec float)` is mis-resolved to
             * `(Vec int)` and the next element conflicts. */
            /* Not through the TYVAR wrapper (generic-spec-carrier-crossings):
             * its TYPE is the true one -- the enclosing signature's `A` -- and
             * the call under it is the carrier `int`.  Peeling it pinned
             * `(let [w (vec-new)] (vec-push! w (vec-get v 0)) ...)` to
             * `(Vec int)`, which retyped the generic's result `int` and lost
             * the function-value dispatch at `A := (fn ...)` (a thin call of
             * a fat handle: SIGSEGV). */
            Expr *ae = args[i];
            while (ae && ae->kind == EX_REINTERPRET &&
                   ae->as.reinterpret_.target_kind != TY_TYVAR)
                ae = ae->as.reinterpret_.expr;
            Type at = ae ? ae->type : args[i]->type;
            if (at.kind != TY_UNKNOWN && !call_type_has_named_tyvar(&at)) {
                uint8_t dummy;
                if (!call_find_type_binding(fwd, n_fwd, pf->as.tyvar_.name, &dummy) &&
                    n_fwd < 16) {
                    fwd[n_fwd].name = pf->as.tyvar_.name;
                    fwd[n_fwd].type = at;
                    n_fwd++;
                }
            }
        }
        if (n_fwd > 0) {
            for (uint32_t i = 0; i < n_args; i++) {
                if (!args[i] || args[i]->kind != EX_VAR) continue;
                Binding *vb = args[i]->as.var.binding;
                if (!vb || vb->is_global || vb->is_param) continue;
                if (!call_type_has_named_tyvar(&vb->type)) continue;
                uint32_t pidx = fn_binding->closure_fn_binding ? i + 1 : i;
                if (pidx >= fn_type.as.fn.arity) continue;
                const Type *pf = fn_type.as.fn.arg_full_types[pidx];
                if (!pf || !call_type_has_named_tyvar(pf)) continue;
                Type inst = call_instantiate_type(e, pf, fwd, n_fwd);
                if (!call_type_has_named_tyvar(&inst) && inst.kind == vb->type.kind) {
                    inst.copy_kind = vb->type.copy_kind;
                    inst.substruct = vb->type.substruct;
                    vb->type = inst;
                    args[i]->type = inst;
                }
            }
        }
    }

    /* generic-return-type-not-inferred-from-context: restore the outer
     * expected-type channel for any subsequent elab (e.g. nested let/do
     * propagation by the caller).  Then, if an outer context constrained
     * this call's result type, bind any still-free tyvars in the
     * function's result_full_type against that expected type -- this is
     * how a (defn [A] [] :A ...) gets specialized when only the return
     * position carries A. */
    e->expected_type = saved_expected_return;
    /* Only bind from the expected return type when it is ground -- i.e.
     * contains no named tyvars itself.  Inside a generic body, the enclosing
     * defn's return-position push carries that body's own tyvars (e.g. `(Map
     * K V)`); registering K->K, V->V bindings here would mark the inner call
     * as "specialized" without any concrete substitution, suppressing the
     * carrier emission that other carrier callers still need.  A ground
     * expected return (`Pos`, `int`, `(Map cstr int)`) is the only safe
     * case to bind from. */
    /* concrete-result-fn-passed-where-an-any-result-fn-is-expected (the
     * generic spelling): not for a call through a LOCAL function value.  Its
     * type variables are the ENCLOSING definition's own -- `g : (fn [float]
     * B)` in `(defn ap1 [B] [g ...] : any (g 1.5))` -- which are rigid there,
     * and binding `B := any` from the `: any` return typed the call `any`
     * while the function the caller passed returns its own `B` (a double):
     * the result was read as a tagged word.  Left open, the call is typed
     * `B`, the return widens it, and the spec's binding tags it. */
    if (saved_expected_return && fn_type.kind == TY_FN &&
        fn_binding && fn_binding->is_global &&
        fn_type.as.fn.result_full_type &&
        call_type_has_named_tyvar(fn_type.as.fn.result_full_type) &&
        !call_type_has_named_tyvar(saved_expected_return)) {
        (void)call_collect_type_bindings(fn_type.as.fn.result_full_type,
                                         *saved_expected_return,
                                         type_bindings, &n_type_bindings);
    }
    /* defopaque-struct-payload-fails-through-unsafe-helper: a
     * return-only-polymorphic callee -- one whose RESULT is a bare type
     * variable and none of whose arguments carry it (e.g. an inline-C
     * `(defn __get [A] [b : int idx : int] : A ...)`) -- gets no
     * argument-derived bindings.  When such a call sits in the return
     * position of an *enclosing* generic body, `saved_expected_return` is
     * that body's own tyvar (non-ground), so the ground-only branch above
     * declines to bind and the call reaches emit with zero abi_bindings.
     * emit_abi_register_call then early-returns and the callee is emitted
     * once on the int64 carrier -- silently miscompiling a by-value struct
     * result (the call site expects `Pos`, the carrier returns `int64_t`).
     *
     * Record the callee-result-tyvar -> caller-tyvar mapping so emit can
     * compose it through the active specialization's concrete bindings and
     * mint a per-instantiation clone.  Gated to a BARE-tyvar result so the
     * compound non-ground case the branch above guards against (e.g. a
     * `(Map K V)` relay return) is untouched and keeps the carrier path. */
    else if (saved_expected_return && fn_type.kind == TY_FN &&
             fn_type.as.fn.result_kind == TY_TYVAR &&
             fn_type.as.fn.result_full_type &&
             fn_type.as.fn.result_full_type->kind == TY_TYVAR &&
             saved_expected_return->kind == TY_TYVAR &&
             n_type_bindings == 0) {
        (void)call_collect_type_bindings(fn_type.as.fn.result_full_type,
                                         *saved_expected_return,
                                         type_bindings, &n_type_bindings);
    }
    /* option-consumer-retype-byvalue step 2 (0-arg constructor abi_bindings):
     * a `^construct` constructor whose declared result is a parameterised
     * TY_APP (`none : (Option A)`, `err : (Result A B)`) and that takes no
     * argument carrying its result tyvar gets no argument-derived bindings.
     * When such a call sits in the return position of an *enclosing* generic
     * body, `saved_expected_return` is that body's own tyvar-app (non-ground,
     * e.g. `(Option B)`), so the ground-only branch above declines and the
     * call reaches emit with zero abi_bindings -- `emit_abi_register_call`
     * early-returns and the constructor is emitted once on the int64 carrier,
     * so its by-value-result consumer (the `option-map`/`result-map` body that
     * returns `(none)` in the false arm) cannot assign the carrier box to the
     * by-value `Option__B` return.
     *
     * Record the constructor-result-tyvar -> caller-tyvar mapping (e.g.
     * none's `A` -> option-map's `B`).  emit composes it through the active
     * specialization's concrete bindings (B -> int) so `construct_recovered_-
     * byvalue` mints a per-instantiation clone returning `Option__int` by
     * value.  Gated to `^construct` callees so the broad non-constructor
     * relay case (which the ground-only guard above deliberately keeps on the
     * carrier) is untouched. */
    else if (saved_expected_return && fn_type.kind == TY_FN &&
             fn_binding && fn_binding->is_construct_template &&
             fn_type.as.fn.result_full_type &&
             call_type_has_named_tyvar(fn_type.as.fn.result_full_type) &&
             call_type_has_named_tyvar(saved_expected_return) &&
             n_type_bindings == 0) {
        (void)call_collect_type_bindings(fn_type.as.fn.result_full_type,
                                         *saved_expected_return,
                                         type_bindings, &n_type_bindings);
    }
    /* zero-arg-construct-ground-byvalue-return: the GROUND counterpart of the
     * step-2 branch above.  A 0-arg `^construct` constructor whose declared
     * result is a parameterised TY_APP (`none : (Option A)`) sitting in a
     * return position whose expected type is a *ground* (tyvar-free) TY_APP
     * (`(Option BoundedIdx)` -- a monomorphic defn's declared return) gets no
     * argument-derived bindings AND no tyvar to compose through, so the
     * step-2 branch (which requires a named tyvar on BOTH sides) declines and
     * the call reaches emit on the int64 carrier base.  emit then assigns the
     * carrier `none()` into a by-value `Option__BoundedIdx` slot -- a `cc`
     * "incompatible types" error.
     *
     * There is no tyvar to substitute here, just a direct structural match:
     * bind the constructor's result tyvar straight to the ground element
     * (`A -> BoundedIdx`) so `emit_abi_register_call` mints (and records the
     * call Expr* against) a by-value `none__spec__Option__BoundedIdx` clone.
     * Gated to a GROUND expected return so a tyvar-bearing context keeps using
     * the step-2 (compose-through-spec) path, and to `^construct` callees so
     * a plain relay return is untouched. */
    else if (saved_expected_return && fn_type.kind == TY_FN &&
             fn_binding && fn_binding->is_construct_template &&
             fn_type.as.fn.result_full_type &&
             call_type_has_named_tyvar(fn_type.as.fn.result_full_type) &&
             saved_expected_return->kind == TY_APP &&
             !call_type_has_named_tyvar(saved_expected_return) &&
             call_app_has_struct_elem(saved_expected_return) &&
             n_type_bindings == 0) {
        (void)call_collect_type_bindings(fn_type.as.fn.result_full_type,
                                         *saved_expected_return,
                                         type_bindings, &n_type_bindings);
    }
    /* generic-call-result-leaks-callee-tyvar-names: a call whose arguments
     * bound SOME of its result variables but not all -- `(ok (f v))` binds
     * `ok`'s A and leaves its B, `(err e)` the reverse.  The branches above
     * all decline (the expected type is the enclosing body's own `(Result B
     * E)`, and arguments did bind something), so the unbound variable
     * survived the instantiation below BY NAME and read as the caller's
     * same-named variable: inside `result-map [A B E]`, `(ok (f v))` was
     * `(Result B B)` and `(err e)` was `(Result A E)`, and each float spec
     * assigned a `Result__float__float` into the match's `Result__int__int`
     * temp (a cc error).  Bind only the still-unbound variables, from the
     * expected type, on a scratch set adopted whole or not at all. */
    else if (saved_expected_return && fn_type.kind == TY_FN &&
             fn_binding && fn_binding->is_global &&
             n_type_bindings > 0 &&
             fn_type.as.fn.result_full_type &&
             fn_type.as.fn.result_full_type->kind == TY_APP &&
             saved_expected_return->kind == TY_APP &&
             !type_has_open_slot(saved_expected_return)) {
        const char *rnames[16];
        uint8_t n_rnames = 0;
        call_collect_tyvar_names(fn_type.as.fn.result_full_type, rnames, &n_rnames, 16);
        bool any_unbound = false;
        for (uint8_t ri = 0; ri < n_rnames && !any_unbound; ri++) {
            uint8_t idx = 0;
            if (!call_find_type_binding(type_bindings, n_type_bindings, rnames[ri], &idx))
                any_unbound = true;
        }
        if (any_unbound) {
            CallTypeBinding scratch[16];
            uint8_t n_scratch = n_type_bindings;
            for (uint8_t s = 0; s < n_scratch; s++) scratch[s] = type_bindings[s];
            if (call_collect_type_bindings(fn_type.as.fn.result_full_type,
                                           *saved_expected_return,
                                           scratch, &n_scratch)) {
                for (uint8_t s = 0; s < n_scratch; s++) type_bindings[s] = scratch[s];
                n_type_bindings = n_scratch;
            }
        }
    }

    /* D8 Q3, the third face of one Saffron rule ("an undetermined type
     * argument is `any`"): a RESULT type variable that no argument bound.
     * `(none)` is `[A] : (Option A)` with no parameter at all, so neither the
     * ctor widen nor the generic-parameter widen has anything to act on, and
     * the call's result stayed an open `(Option A)` -- tagged under a key no
     * registry row can match, and lowered through the carrier base rather than
     * the `(Option any)` spec the rest of the Saffron program uses.  Bind each
     * still-open result variable to `any` here, BEFORE the instantiation below,
     * so the same code substitutes the result and records the binding as an
     * abi_binding -- which is what makes the emitter mint
     * `none__spec__tur_adt_Option__any` instead of calling the base.
     *
     * Deferred to an enclosing ascription like the other two: `(:: (none)
     * (Option float))` pins the instantiation on purpose. */
    /* saffron-open-generic-result-not-grounded: the TOP-LEVEL form's dialect
     * counts too, as it does for the seams (M10).  `[]` lowers through the
     * `vec-of` macro to a `(vec-new)` carrying stdlib/vec.tur's span, so
     * gating on the call's own span left the empty literal an open `(Vec A)`
     * -- the one Saffron vector that was not `(Vec any)`, whose box then
     * matched no registry row (`(.kind-of x)` on it panicked compiled). */
    if (fn_type.kind == TY_FN && fn_type.as.fn.result_full_type &&
        (lang_span_is_dynamic(call->span) || e->toplevel_dynamic) &&
        call_type_has_named_tyvar(fn_type.as.fn.result_full_type) &&
        !(saved_expected_return && saved_expected_return->kind == TY_APP)) {
        const char *open_names[16];
        uint8_t n_open = 0;
        call_collect_tyvar_names(fn_type.as.fn.result_full_type, open_names, &n_open, 16);
        for (uint8_t oi = 0; oi < n_open && n_type_bindings < 16; oi++) {
            bool bound = false;
            for (uint8_t bi = 0; bi < n_type_bindings; bi++)
                if (type_bindings[bi].name && strcmp(type_bindings[bi].name, open_names[oi]) == 0) {
                    bound = true; break;
                }
            if (bound) continue;
            type_bindings[n_type_bindings].name = open_names[oi];
            type_bindings[n_type_bindings].type = type_from_kind(TY_ANY);
            n_type_bindings++;
        }
    }
    /* Result type is the function's return type */
    Type result_type;
    if (fn_type.kind == TY_FN) {
        TypeKind result_kind = fn_type.as.fn.result_kind;
        /* Preserve the full return type payload when available.
         * This keeps compound returns such as Session[P], Role[...], and
         * higher-order TY_FN results from collapsing into a bare TypeKind shell. */
        if (fn_type.as.fn.result_full_type) {
            if (call_type_has_named_tyvar(fn_type.as.fn.result_full_type) &&
                n_type_bindings > 0) {
                result_type = call_instantiate_type(e, fn_type.as.fn.result_full_type,
                                                    type_bindings, n_type_bindings);
                /* generic-call-result-leaks-callee-tyvar-names: a result
                 * variable nothing bound -- not an argument, not the expected
                 * type above -- is still the CALLEE's, so it must not read as
                 * the enclosing signature's variable of the same spelling:
                 * `(ok x)` inside `[A B E]` was `(Result B B)`.  Rename it
                 * apart (`ok.B`, an open slot) in the call's type only; the
                 * ABI bindings, which say what the call was specialized at,
                 * are untouched.  Not for a recursive call, whose variables
                 * ARE the enclosing signature's. */
                if (fn_binding && fn_binding->is_global && fn_binding->name &&
                    fn_binding->name != e->current_fn_name &&
                    e->n_sig_tyvars > 0) {
                    const char *rn[16];
                    uint8_t n_rn = 0;
                    call_collect_tyvar_names(fn_type.as.fn.result_full_type, rn, &n_rn, 16);
                    /* One instantiation of the callee's own result over both
                     * sets: renaming the already-substituted type would also
                     * rename a caller variable a binding put there. */
                    CallTypeBinding apart[32];
                    uint8_t n_apart = 0;
                    for (uint8_t bi = 0; bi < n_type_bindings; bi++)
                        apart[n_apart++] = type_bindings[bi];
                    uint8_t n_bound = n_apart;
                    for (uint8_t ri = 0; ri < n_rn && n_apart < 32; ri++) {
                        uint8_t idx = 0;
                        if (call_find_type_binding(type_bindings, n_type_bindings, rn[ri], &idx))
                            continue;
                        if (!ng_tyvar_in_sig(e, rn[ri])) continue;
                        char nm[96];
                        snprintf(nm, sizeof nm, "%s.%s", fn_binding->name->name, rn[ri]);
                        const Symbol *ns = symtab_intern(e->st, strslice(nm, (uint32_t)strlen(nm)));
                        apart[n_apart].name = rn[ri];
                        apart[n_apart].type = type_tyvar_named(ns->name);
                        apart[n_apart].type.as.tyvar_.open_slot = true;
                        n_apart++;
                    }
                    if (n_apart > n_bound)
                        result_type = call_instantiate_type(e, fn_type.as.fn.result_full_type,
                                                            apart, n_apart);
                }
            } else {
                result_type = *fn_type.as.fn.result_full_type;
            }
        } else {
            result_type = type_from_kind(result_kind);
        }
    } else if (fn_type.kind == TY_CONT) {
        /* Calling a continuation returns its result type (though in practice it jumps) */
        result_type = type_from_kind(fn_type.as.cont.returns);
    } else {
        result_type = TYPE_NIL;
    }

    Type call_result_type = result_type;
    bool wrap_generic_result = false;
    if (fn_type.kind == TY_FN &&
        fn_type.as.fn.result_kind == TY_TYVAR) {
        /* The declared result is a bare type variable, lowered to the int64
         * carrier at the ABI level.  When the instantiation resolves to a
         * scalar (float/cstr/bool/...), keep the call's C type as the int64
         * carrier and record a reinterpret so emit bitcasts it back to the
         * scalar's register class.  But when it resolves to a *concrete
         * composite* carrier-ABI type (a parameterised TY_APP, or a monomorphic
         * struct/ADT with a def), the carrier IS already int64 and the full
         * type must be preserved for downstream type checking: collapsing to
         * int discards the payload (e.g. (Tuple2 cstr int) -> int) and breaks
         * chained generic accessors like (tuple2-2nd (tuple2-2nd t)).  A
         * reinterpret cannot carry a composite anyway -- type_size_bytes is 0
         * for TY_APP/struct/ADT, so call_wrap_reinterpret would no-op and the
         * collapse to int would be pure loss.  See
         * docs/archive/history/polymorphic-return-type-instantiation-collapses-to-first-tyvar.md
         *
         * vec-get-existential-element-erased-to-int: an existential (or its
         * dual forall) is the same shape of carrier-ABI structural type -- a
         * `Vec (exists [a] [(C a)] a)` read through `(vec-get v i) : A`
         * instantiates A to the existential, whose carrier is already a
         * pointer-width int64.  Collapsing it to int erases the
         * `as.forall_.body` payload that `open` (and every existential op) must
         * see, forcing a per-read `(:: ... (exists ...))` re-ascription.  Keep
         * the full type for the same reason as the composites above. */
        /* hkt-cata-function-typed-carrier-not-threaded: a function-typed
         * carrier (a fold whose B is itself `(fn ...)` -- a CPS matcher, an
         * environment-passing interpreter, a `Doc = (fn ...)` pretty-printer)
         * resolves the bare-tyvar result to a TY_FN.  A function value is
         * pointer-width and already inhabits the int64 carrier register class
         * (rax, not xmm0), exactly like the TY_APP/struct/ADT composites above,
         * so collapsing it to int is pure loss: it erases the arg/result
         * payload the call site needs to APPLY the returned function, and the
         * application fails with "expression in call head has type `int`, which
         * is not callable".  Keep the full TY_FN for the same reason. */
        /* saffron-lang-plan S6: `any` and `union` join the composites, and for
         * the reason the comment above already gives for them -- "a reinterpret
         * cannot carry a composite anyway".  Both are the TWO-WORD
         * `tur_tagged_t`, so there is no single carrier word to bitcast back
         * from: collapsing to int keeps the payload and drops the tag, which is
         * how `(vec-get v 0)` on a `(Vec any)` came back typed `int` and
         * `type-of` on it was a compile error naming that.  A boxed element
         * read derefs, exactly as it does for the by-value aggregates above. */
        bool result_is_concrete_composite =
            (result_type.kind == TY_APP) ||
            (result_type.kind == TY_ADT && result_type.as.adt_.def) ||
            (result_type.kind == TY_EXISTS) ||
            (result_type.kind == TY_FORALL) ||
            (result_type.kind == TY_ANY) ||
            (result_type.kind == TY_UNION) ||
            (result_type.kind == TY_FN);
        if (!result_is_concrete_composite) {
            call_result_type = TYPE_INT;
            wrap_generic_result = (result_type.kind != TY_INT);
        }
    }

    /* poly-closure-result-specialization (Stage B-D): a generic closure-returning
     * defn specialized at a FLOAT result type previously always raised TUR-E0705
     * (the lifted inner body was emitted once on the integer thunk ABI -- a
     * xmm0-vs-rax register-class miscompile).  The emit phase now clones the
     * inner body per monomorphization (emit_module.c
     * emit_inner_closure_needs_float_spec + EmitAbiSpecialization.env_name_-
     * override / inner_closure_spec_idx), so a DISPATCH-FREE inner body (e.g.
     * `(fn [t] : A val)` returning a captured value) is register-class-correct.
     *
     * The guard is RETAINED only for the case emit cannot yet fix: an inner body
     * that fat-dispatches a captured closure (e.g. `(fn [x] (g (f x)))`).  There
     * the intermediate result types are erased to the int64 carrier in the
     * elaborated body, so the clone cannot recover their float register class --
     * a hard error is correct, not a silent miscompile.  Generalizing this
     * (per-spec re-elaboration of the inner body) is the remaining Stage E work;
     * see docs/archive/history/poly-closure-inner-dispatch-result-erased.md. */
    /* poly-closure-inner-dispatch-result-erased: fire E0705 only when the
     * dispatching inner body has untyped fat-calls that Direction 3 cannot
     * handle.  When all dispatches are through typed (fn [..] R) bindings with
     * named-tyvar result types, emit_expr.c Direction 3 derives the correct C
     * dispatch type and the guard is no longer needed. */
    if (n_type_bindings > 0 && fn_binding &&
        fn_binding->returns_closure_fn_binding &&
        fn_binding->closure_return_dispatches_untyped) {
        Binding *inner = fn_binding->returns_closure_fn_binding;
        const Type *inner_res = (inner->type.kind == TY_FN)
            ? inner->type.as.fn.result_full_type : NULL;
        if (inner_res && inner_res->kind == TY_TYVAR && inner_res->as.tyvar_.name) {
            uint8_t bidx = 0;
            if (call_find_type_binding(type_bindings, n_type_bindings,
                                       inner_res->as.tyvar_.name, &bidx)) {
                TypeKind bk = type_bindings[bidx].type.kind;
                if (bk == TY_FLOAT || bk == TY_FLOAT32 || bk == TY_FLOAT64) {
                    diag_emit_with_code(DIAG_ERROR, call->span,
                        TUR_E0705_POLY_CLOSURE_RESULT_TYVAR,
                        "polymorphic closure-returning function '%s' specialized at a "
                        "floating-point type (TUR-E0705): the returned (fn ...) result "
                        "type is the type parameter '%s', and its body dispatches a "
                        "captured closure whose intermediate result types are erased to "
                        "the integer-register closure ABI; a float specialization would "
                        "be a register-class miscompile (xmm0 vs rax). Work around by "
                        "writing a monomorphic defn per concrete result type.",
                        fn_binding->name ? fn_binding->name->name : "?",
                        inner_res->as.tyvar_.name);
                }
            }
        }
    }

    /* bare-fat-result-monomorphization (Phase B): a deferred (lazy) bare-^fat
     * callee has no canonical body -- its `(g x)` only types once the incoming
     * closure's result kind is known.  Recover that kind from the bare-^fat
     * argument slot and redirect the call to a per-call-site clone.  Non-lazy
     * callees (the common int-carrier case, including Phase A float fixtures)
     * are untouched, so existing codegen/snapshots do not change. */
    Binding *bound_fn = fn_binding;
    if (fn_binding && fn_binding->bare_fat_lazy &&
        fn_binding->type.kind == TY_FN) {
        for (uint32_t i = 0; i < n_args; i++) {
            if (i >= fn_binding->type.as.fn.arity ||
                !FN_ARG_FLAG(fn_binding->type.as.fn, i, FA_FAT))
                continue;
            TypeKind rk = bare_fat_arg_result_kind(args[i]);
            if (rk != TY_UNKNOWN) {
                Binding *sp = elab_specialize_bare_fat(e, fn_binding, rk);
                if (sp) bound_fn = sp;
            }
            break;  /* the first bare-^fat slot drives specialization */
        }
        if (bound_fn == fn_binding) {
            /* A lazy bare-^fat callee has no canonical body; if we could not
             * recover the closure's result kind (e.g. a self-recursive call
             * passing the bare-^fat param itself, which carries no signature)
             * the call cannot be specialized.  Recursion / mutual recursion is
             * a deferred open issue in the plan -- emit a clear error rather
             * than a body-less symbol that fails at link time. */
            diag_emit(DIAG_ERROR, call->span,
                "cannot specialize bare-^fat function '%s' at this call site: "
                "the closure's result kind is not recoverable here "
                "(recursive / non-literal bare-^fat results are not yet supported)",
                fn_binding->name->name);
            return NULL;
        }
    }

    /* sized-types-cross-param-unification: reject statically-known size
     * disagreements between args that share a size variable in the callee's
     * signature (e.g. two `(SizedVec n)` parameters passed lengths 2 and 3).
     * Runs after arg elaboration so each arg carries its inferred SZ8 size
     * index.  No effect on calls to non-sized signatures. */
    if (sz_cross_param_unify(e, call, &fn_type, fn_binding, args, n_args))
        return NULL;
    /* gadt-length-index-not-enforced: the same, for the phantom index of a
     * GADT argument -- `(vec-head (VNil))` against `[v : (Vec (Succ n))]`. */
    if (gadt_index_check_call(e, call, &fn_type, fn_binding, args, n_args))
        return NULL;

    /* class-defn-constraint-not-discharged-at-call-site: a defn carrying a
     * typeclass constraint (`^Encode T`, or the `[(Encode T)]` middle-vector
     * form) is checked abstractly inside its own body, but that obligation must
     * be RE-discharged at each instantiating call site.  For every constraint
     * whose type variable is pinned to a concrete type by this call's arguments
     * (type_bindings), verify a matching instance exists; a missing instance is
     * a hard error here rather than a deferred emit/link failure or a silently
     * wrong dispatch.  A constraint whose tyvar is not argument-pinned (resolved
     * only through the return context, or still abstract because the call sits
     * inside another generic body) is left to the existing machinery -- the
     * concrete type is not known here, so there is nothing to discharge yet. */
    if (fn_binding && fn_binding->fn_constraints &&
        fn_binding->fn_constraints->n_constraints > 0 && n_type_bindings > 0) {
        const ConstraintSet *cs = fn_binding->fn_constraints;
        for (uint8_t ci = 0; ci < cs->n_constraints; ci++) {
            const TypeConstraint *con = &cs->constraints[ci];
            if (!con->typeclass || !con->tyvar || !con->tyvar->name) continue;
            uint8_t bidx = 0;
            if (!call_find_type_binding(type_bindings, n_type_bindings,
                                        con->tyvar->name, &bidx)) continue;
            Type concrete = type_bindings[bidx].type;
            /* Only discharge against a concrete, ground type.  A tyvar/unknown
             * binding means the obligation is still abstract (the call is itself
             * inside a generic body), so defer to that body's own constraint.
             *
             * Skip an APPLIED type (`(Dense Pos)`, TY_APP) too: those resolve
             * against *parametric* instances (`(definstance StorageOps [(Dense
             * A)] ...)`) whose head is itself a TY_APP, which needs the full
             * PTC3/PTC4 instance-constraint-satisfaction machinery that the
             * monomorphization/emit path already runs.  A single-type
             * `typeclass_env_lookup_instance` here would not unify the parametric
             * head and would false-positive on a present instance.  Ground
             * opaque/struct/ADT/primitive receivers (the common case, and the
             * one the http-handler `^Encode T` defns hit) are matched precisely,
             * so this only narrows coverage, never miscatches. */
            if (concrete.kind == TY_TYVAR || concrete.kind == TY_UNKNOWN ||
                concrete.kind == TY_APP) continue;
            TypeClassInstance *inst = typeclass_env_lookup_instance(
                &e->typeclass_env, con->typeclass, &concrete, 1);
            if (!inst) {
                Buf tb; buf_init(&tb);
                type_print(&tb, concrete);
                buf_putc(&tb, '\0');
                diag_emit_with_code(DIAG_ERROR, call->span, TUR_E0001_TYPE_MISMATCH,
                    "no '%s' instance for '%s' in constrained call to '%s': the "
                    "type bound to '%s' has no %s instance, but '%s' requires one",
                    con->typeclass->name->name, tb.data,
                    fn_binding->name ? fn_binding->name->name : "?",
                    con->tyvar->name, con->typeclass->name->name,
                    fn_binding->name ? fn_binding->name->name : "?");
                buf_free(&tb);
                return NULL;
            }
        }
    }

    /* van-laarhoven-lens-composition (Gap B): forward the ENCLOSING constrained
     * rank-2 fn's dict into this nested call to ANOTHER constrained rank-2 fn at
     * the same abstract functor.  Inside `(defn wrap [^f] [^Functor f ...] (point-x
     * g s))`, the call to `point-x` pins its `Functor f` obligation to `wrap`'s
     * own abstract `f` -- deferred above, so the call would otherwise resolve to
     * point-x's plain carrier base (an arbitrary hardcoded instance + a thin
     * fn-ptr call on the boxed `g` -> SIGSEGV).  Redirect to point-x's dict-clone
     * and prepend an AMBIENT dict value, which lowers to `wrap`'s own dict param
     * when `wrap` is itself emitted as a dict-clone. */
    Binding *fwd_bound = bound_fn;
    Expr   **fwd_args  = args;
    uint32_t fwd_nargs = n_args;
    if (e->cur_hkt_constraint_class &&
        e->cur_hkt_constraint_tyvar && fn_binding && bound_fn == fn_binding &&
        fn_binding->fn_constraints &&
        fn_binding->fn_constraints->n_constraints == 1 &&
        fn_binding->fn_constraints->constraints[0].typeclass ==
            e->cur_hkt_constraint_class &&
        fn_binding->source_fn_def) {
        const TypeConstraint *con0 = &fn_binding->fn_constraints->constraints[0];
        uint8_t bidx0 = 0;
        bool pins_ambient = false;
        if (con0->tyvar && con0->tyvar->name &&
            call_find_type_binding(type_bindings, n_type_bindings,
                                   con0->tyvar->name, &bidx0)) {
            Type c0 = type_bindings[bidx0].type;
            pins_ambient = (c0.kind == TY_TYVAR && c0.as.tyvar_.name &&
                            strcmp(c0.as.tyvar_.name,
                                   e->cur_hkt_constraint_tyvar) == 0);
        }
        if (pins_ambient) {
            Binding *clone = make_dict_clone(e, fn_binding, call->span);
            if (clone) {
                /* Reference the enclosing fn's synthetic ambient-dict binding as
                 * the leading dict arg.  As a real binding it is captured by an
                 * adapter lambda's free-var scan (Gap B2), so the lifted lambda
                 * forwards the caller's actual dict; directly in the caller's own
                 * dict-clone body it lowers to that clone's dict parameter. */
                Expr *amb;
                if (e->cur_hkt_dict_binding) {
                    amb = expr_new(e->arena, EX_VAR,
                                   e->cur_hkt_dict_binding->type, call->span);
                    amb->as.var.binding = e->cur_hkt_dict_binding;
                } else {
                    TypeClassInstance *repr = NULL;
                    for (TypeClassInstance *it = e->typeclass_env.instances; it;
                         it = it->next)
                        if (it->typeclass == e->cur_hkt_constraint_class) {
                            repr = it; break;
                        }
                    amb = expr_new(e->arena, EX_DICT, TYPE_PTR_VOID, call->span);
                    amb->as.dict_.instance = repr;
                    amb->as.dict_.method_name[0] = '\0';
                    amb->as.dict_.is_ambient = true;
                }
                Expr **na = (Expr **)arena_alloc(e->arena,
                                                 (n_args + 1) * sizeof(Expr *));
                na[0] = amb;
                for (uint32_t k = 0; k < n_args; k++) na[k + 1] = args[k];
                fwd_bound = clone;
                fwd_args  = na;
                fwd_nargs = n_args + 1;
            }
        }
    }

    /* Route B (constrained-hkt-lifted-lambda-keeps-representative-instance):
     * a DIRECT call to a constrained HIGHER-KINDED poly fn at a concrete type
     * constructor -- `(bind-then-pure (some 41))` from an unconstrained caller.
     * Monomorphizing this left every class-method call in the body resolved
     * against the representative instance the elaborator baked; emit-side
     * re-resolution repairs the body's own calls but cannot reach a lifted
     * continuation, whose `pure` kept the representative for good.
     *
     * Route the call through the callee's DICT CLONE instead, passing the
     * concrete instances' dict singletons.  Every method call in the body --
     * receiver- or return-directed, inline or in a lifted continuation (which
     * captures the dict in its closure env via the nested-mapper lowering) --
     * then loads its target out of a dictionary, correct by construction with
     * no dependence on specialization.  This is the same lowering the rank-2
     * forall path uses.
     *
     * Gated to HIGHER-KINDED constraints only: kind-`*` constrained defns (the
     * Dec/Enc/Tag family) keep monomorphization, which their by-value element
     * specialization depends on.  Fires only when the ambient forwarding above
     * did not already claim the call and every constraint grounds to a concrete
     * instance; otherwise the call falls through to the existing paths. */
    bool dict_clone_byvalue_result = false;
    if (fwd_bound == bound_fn && fn_binding && bound_fn == fn_binding &&
        fn_binding->fn_constraints &&
        fn_binding->fn_constraints->n_constraints >= 1 &&
        fn_binding->fn_constraints->n_constraints <= MAX_FN_CONSTRAINTS &&
        fn_binding->source_fn_def && n_type_bindings > 0) {
        const ConstraintSet *cs = fn_binding->fn_constraints;
        TypeClassInstance *insts[MAX_FN_CONSTRAINTS];
        uint8_t nc = cs->n_constraints;
        bool all_hkt_concrete = true;
        for (uint8_t ci = 0; ci < nc && all_hkt_concrete; ci++) {
            const TypeConstraint *con = &cs->constraints[ci];
            insts[ci] = NULL;
            if (!con->typeclass || !con->tyvar || !con->tyvar->name) {
                all_hkt_concrete = false; break;
            }
            bool is_hkt = false;
            if (con->typeclass->type_param_kinds)
                for (uint8_t k = 0; k < con->typeclass->n_type_params; k++)
                    if (con->typeclass->type_param_kinds[k] != KIND_STAR) {
                        is_hkt = true; break;
                    }
            if (!is_hkt) { all_hkt_concrete = false; break; }
            uint8_t bidx = 0;
            if (!call_find_type_binding(type_bindings, n_type_bindings,
                                        con->tyvar->name, &bidx)) {
                all_hkt_concrete = false; break;
            }
            /* The binding is the APPLIED type `(Option int)`; the instance head
             * is the bare constructor, so walk the spine to it. */
            Type bt = type_bindings[bidx].type;
            while (bt.kind == TY_APP && bt.as.app.fn) bt = *bt.as.app.fn;
            if (bt.kind == TY_TYVAR || bt.kind == TY_UNKNOWN) {
                all_hkt_concrete = false; break;
            }
            insts[ci] = typeclass_env_lookup_instance(&e->typeclass_env,
                                                      con->typeclass, &bt, 1);
            if (!insts[ci]) { all_hkt_concrete = false; break; }
        }
        if (all_hkt_concrete) {
            Binding *clone = make_dict_clone(e, fn_binding, call->span);
            if (clone) {
                Expr **na = (Expr **)arena_alloc(e->arena,
                                                 (n_args + nc) * sizeof(Expr *));
                for (uint8_t ci = 0; ci < nc; ci++) {
                    Expr *d = expr_new(e->arena, EX_DICT, TYPE_PTR_VOID, call->span);
                    d->as.dict_.instance = insts[ci];
                    d->as.dict_.method_name[0] = '\0';
                    d->as.dict_.is_ambient = false;
                    na[ci] = d;
                }
                for (uint32_t k = 0; k < n_args; k++) na[nc + k] = args[k];
                fwd_bound = clone;
                fwd_args  = na;
                fwd_nargs = n_args + nc;
                /* A dict clone returns the int64 CARRIER for every result
                 * (emit_fns.c forces that off n_dict_clone).  A by-value
                 * aggregate result -- `(Option int)` -- must be bridged back:
                 * type the call as the carrier and ascribe it to the aggregate
                 * so the existing carrier->concrete ascription bridge derefs. */
                if (call_result_type.kind == TY_APP)
                    dict_clone_byvalue_result = true;
            }
        }
    }

    Expr *out = expr_new(e->arena, EX_CALL,
                         dict_clone_byvalue_result ? type_from_kind(TY_INT)
                                                   : call_result_type,
                         call->span);
    out->as.call_.fn_binding = fwd_bound;
    out->as.call_.args = fwd_args;
    out->as.call_.n_args = fwd_nargs;
    out->as.call_.fn_expr = NULL;
    /* GS5/CS3: hand the named-tyvar substitution to emit so it can drive ABI
     * specialization without re-deriving it from the call's argument types. */
    if (n_type_bindings > 0) {
        AbiTypeBinding *saved = (AbiTypeBinding *)arena_alloc(
            e->arena, n_type_bindings * sizeof(AbiTypeBinding));
        for (uint8_t bi = 0; bi < n_type_bindings; bi++) saved[bi] = type_bindings[bi];
        out->as.call_.abi_bindings = saved;
        out->as.call_.n_abi_bindings = n_type_bindings;
    }
    if (dict_clone_byvalue_result) {
        /* Bridge the dict clone's int64 carrier result back to the by-value
         * aggregate the call site expects (see the Route B block above). */
        Expr *asc = expr_new(e->arena, EX_ASCRIBE, call_result_type, call->span);
        asc->as.ascribe_.inner = out;
        return asc;
    }
    if (wrap_generic_result && result_type.kind == TY_TYVAR &&
        result_type.as.tyvar_.name &&
        ng_tyvar_in_sig(e, result_type.as.tyvar_.name)) {
        /* generic-call-result-in-generic-collapses-to-int: the result is the
         * ENCLOSING signature's own type variable -- `(vec-get v 0)` inside
         * `(defn first-of [A] [v : (Vec A)] : A ...)`.  The size-keyed wrap
         * below cannot size a tyvar and silently dropped itself, so the call
         * stayed typed `int` in EVERY position, and everything downstream
         * inferred from that `int`: `(Box (vec-get v 0))` minted `Box__int`
         * in the float spec (7.1's bits printed as 4.61968e+18), `(some ...)`
         * matched as `Option__int` (invalid C), and a bare tail returned the
         * carrier word into a `double` return by VALUE conversion.
         *
         * Wrap it in the reinterpret typed `A` that the `let` position has
         * used since let-bound-generic-call-result-in-generic-truncates
         * (`let_bridge_sig_tyvar_result`): the call node keeps its carrier
         * `int`, the value it hands every consumer is typed `A`, and emit
         * lowers that per clone (the EX_REINTERPRET tyvar arm: identity in
         * the base, carrier->concrete in a spec when the call returned the
         * carrier word).  One decision at the producer instead of one per
         * consumer. */
        Expr *r = expr_new(e->arena, EX_REINTERPRET, result_type, call->span);
        r->as.reinterpret_.expr = out;
        r->as.reinterpret_.source_kind = TY_INT;
        r->as.reinterpret_.target_kind = TY_TYVAR;
        r->as.reinterpret_.retain = false;
        return r;
    }
    if (wrap_generic_result) {
        return call_wrap_reinterpret_owning(
            e, out, result_type.kind, call->span,
            own_carry_for_result(fn_binding && fn_binding->name
                                     ? fn_binding->name->name : NULL));
    }
    return out;
}

/* Phase 2 wrapper: elaborate the call, then apply the closure-drop-glue S1
 * borrowed-closure hoist to the result (a no-op unless the call passes an inline
 * capturing closure to a `^borrow` fn-param). */
/* closure-capture-escapes-linearity: invoking a linear callable CONSUMES it.
 *
 * A call in head position does not go through the general var-use path in
 * elab_toplevel.c, which is what marks a linear binding consumed and rejects the
 * second use.  Without this, a closure that inherited linearity from a consumed
 * capture was reported as *dropped* (TUR-E0100) even when it was called -- the
 * mark reached the binding but nothing ever discharged it.
 *
 * Continuations are excluded: `(k v)` on a TY_CONT / is_continuation binding is
 * application sugar with its own consume-and-check in elab_call_fn_inner, and
 * running both would consume here and then report a spurious use-after-consume
 * there. */
static bool call_consume_linear_callable(Binding *fn_binding, const Form *call) {
    if (!fn_binding) return true;
    if (fn_binding->is_continuation || fn_binding->type.kind == TY_CONT) return true;
    if (fn_binding->is_unique) {
        /* The unique half: a second invocation is a second use of a
         * use-at-most-once value, which is E0201 (same code the general var-use
         * path raises for `(f)` in non-head position). */
        if (fn_binding->is_moved) {
            diag_emit_with_code(DIAG_ERROR, call->span,
                                TUR_E0201_UNIQUE_COPY,
                                "cannot copy unique value '%s' -- "
                                "unique values may be used at most once",
                                fn_binding->name->name);
            return false;
        }
        fn_binding->is_moved = true;
    }
    if (!fn_binding->is_linear) return true;
    if (fn_binding->is_linear_consumed) {
        diag_emit_with_code(DIAG_ERROR, call->span,
                            TUR_E0101_LINEAR_USE_AFTER_CONSUME,
                            "linear value '%s' used after being consumed",
                            fn_binding->name->name);
        return false;
    }
    fn_binding->is_linear_consumed = true;
    return true;
}

static Expr *elab_call_fn(Elab *e, const Form *call, Binding *fn_binding) {
    if (!call_consume_linear_callable(fn_binding, call)) return NULL;
    Expr *r = elab_call_fn_inner(e, call, fn_binding);
    return r ? hoist_borrowed_closure_args(e, r, call->span) : r;
}

/* forall-dict-pass-multi-constraint-hkt-plan (Task 3.1 residual guard): a
 * class-method call on a constrained type variable (an int-representative
 * dispatch tagged with a `dict_arg` whose class is one of the clone's
 * constraints) is only re-routed through the runtime dict param when it is
 * emitted INSIDE the dict-clone body proper.  A nested lambda argument -- a
 * van Laarhoven mapper `(fn [x] (show x))` passed to `fmap` -- is lifted to
 * its own top-level C function with NO dict param in scope, so its `show x`
 * would silently mis-resolve to the carrier-representative instance (e.g.
 * `Show int` on a bool).  Detect that shape here so the call site can reject
 * it with a specific diagnostic instead of miscompiling.
 *
 * `inside_lambda` starts false at the body root and flips true when we descend
 * into a lifted lambda: either an inline EX_FN body or -- the common case --
 * the FnDef reached through an EX_VAR that references a captureless lifted
 * lambda (`is_lifted_lambda`), which is how a mapper argument reaches the body.
 * `depth` bounds the lifted-lambda chain so a self/mutually-recursive lifted
 * lambda cannot loop the walk. */
static const TypeClass *call_dispatched_constraint_class(const Expr *e,
                                                         const ConstraintSet *cs);

static bool dict_clone_nested_dispatch_rec(const Expr *e,
                                           const ConstraintSet *cs,
                                           bool inside_lambda, int depth) {
    if (!e || !cs || depth > 32) return false;
#define REC(x) dict_clone_nested_dispatch_rec((x), cs, inside_lambda, depth)
    switch (e->kind) {
        case EX_CALL: {
            /* A constraint-var method dispatch: tagged with a dict_arg whose
             * class is one of the clone's constraints, and whose receiver is a
             * bare type variable (the constraint's own var, resolved to the
             * carrier representative).  A concrete-typed receiver re-resolves
             * correctly inside a lifted lambda and must NOT be rejected. */
            if (inside_lambda && call_dispatched_constraint_class(e, cs))
                return true;
            if (e->as.call_.fn_expr && REC(e->as.call_.fn_expr)) return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (REC(e->as.call_.args[i])) return true;
            return false;
        }
        case EX_VAR: {
            /* A mapper argument is lifted to a top-level `__fn_N` and reaches
             * the body as an EX_VAR whose binding carries the lifted FnDef.
             * Descend into that FnDef's body as a nested lambda. */
            const Binding *vb = e->as.var.binding;
            if (vb && vb->is_lifted_lambda && vb->source_fn_def &&
                vb->source_fn_def->body)
                return dict_clone_nested_dispatch_rec(vb->source_fn_def->body,
                                                      cs, true, depth + 1);
            return false;
        }
        case EX_FN:
            return e->as.fn_.fn &&
                   dict_clone_nested_dispatch_rec(e->as.fn_.fn->body, cs, true,
                                                  depth + 1);
        case EX_FN_DEF:
            return e->as.fn_def_.fn &&
                   dict_clone_nested_dispatch_rec(e->as.fn_def_.fn->body, cs,
                                                  true, depth + 1);
        case EX_LET:
        case EX_LETREC: {
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (REC(e->as.let_.bindings[i].init)) return true;
            return REC(e->as.let_.body);
        }
        case EX_IF:
            return REC(e->as.if_.cond) || REC(e->as.if_.then_) ||
                   REC(e->as.if_.else_or_null);
        case EX_DO: {
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (REC(e->as.do_.items[i])) return true;
            return false;
        }
        case EX_WHILE:
            return REC(e->as.while_.cond) || REC(e->as.while_.body);
        case EX_MATCH: {
            if (REC(e->as.match_.scrutinee)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (REC(e->as.match_.arms[i].body)) return true;
                if (REC(e->as.match_.arms[i].guard)) return true;
            }
            return false;
        }
        case EX_POLY_WRAP:  return REC(e->as.poly_wrap_.inner);
        case EX_FN_TO_FAT:  return REC(e->as.fn_to_fat_.inner);
        case EX_ASCRIBE:    return REC(e->as.ascribe_.inner);
        case EX_CAST:        return REC(e->as.cast_.expr);
        case EX_REINTERPRET: return REC(e->as.reinterpret_.expr);
        default:            return false;
    }
#undef REC
}

static bool dict_clone_dispatch_in_nested_lambda(const Expr *e,
                                                 const ConstraintSet *cs,
                                                 bool inside_lambda) {
    return dict_clone_nested_dispatch_rec(e, cs, inside_lambda, 0);
}

/* forall-dict-pass-nested-lambda-dispatch-plan (Phase 2) +
 * forall-dict-pass-nested-mapper-general-plan (Phases 1 + 3): scan a mapper
 * lambda's OWN body for the constraint class(es) it dispatches directly on its
 * tyvar receiver.  ACCUMULATES the set of dispatched constraint classes (dedup
 * by class identity) into `out_classes[0..*n_out)`.  Traverses structural nodes
 * (calls, lets, ifs, ...) but STOPS at every lambda boundary (an EX_VAR to a
 * lifted lambda, an inline EX_FN, a poly-wrap, or a closure): a dispatch buried
 * inside a DEEPER lambda is that lambda's OWN dispatch and is lowered by the
 * recursive walk (dict_clone_lower_nested_mappers) descending into it, not
 * hoisted here.  A dispatch reached through a lambda boundary this scan cannot
 * cross (a direct-call lifted lambda) simply is not found here and falls through
 * to the TUR-E0311 guard. */
/* Route B (constrained-hkt-lifted-lambda-keeps-representative-instance): does
 * this call dispatch a typeclass method on the clone's constrained type
 * variable?  Two shapes qualify:
 *   - receiver-directed: arg 0's type is a bare TY_TYVAR (`bind`, `fmap`);
 *   - return-directed: the method has no receiver to key on and carries the
 *     constraint var in its RESULT -- a bare TY_TYVAR, or the head of a
 *     `(m b)` TY_APP spine (`pure`, `empty`).
 * Returns the dispatched class when it is one of `cs`'s constraints, else NULL.
 * Shared by the mapper scanner, the E0311 guard, and (mirrored) the emit-side
 * env-dict index so the three never disagree. */
static const TypeClass *call_dispatched_constraint_class(const Expr *e,
                                                         const ConstraintSet *cs) {
    if (!e || e->kind != EX_CALL || !cs) return NULL;
    if (!(e->as.call_.dict_arg && e->as.call_.dict_arg->kind == EX_DICT &&
          e->as.call_.dict_arg->as.dict_.instance &&
          e->as.call_.dict_arg->as.dict_.instance->typeclass))
        return NULL;
    /* hkt-generic-nested-bind-result-type: the receiver of a higher-kinded
     * method is `(m a)`, a TY_APP headed by the constraint var, not a bare
     * TY_TYVAR.  A `bind` on a captured `b : (M int)` inside a `do-m`
     * continuation was therefore not seen as a dispatch: the lambda captured
     * no Monad dict and the call kept the representative instance.  Key on
     * the receiver's HEAD, as the result test below already does. */
    bool recv_is_tyvar = false;
    if (e->as.call_.n_args >= 1 && e->as.call_.args && e->as.call_.args[0]) {
        const Type *rh = &e->as.call_.args[0]->type;
        while (rh->kind == TY_APP && rh->as.app.fn) rh = rh->as.app.fn;
        recv_is_tyvar = (rh->kind == TY_TYVAR);
    }
    bool result_is_tyvar_headed = false;
    {
        const Type *h = &e->type;
        while (h->kind == TY_APP && h->as.app.fn) h = h->as.app.fn;
        result_is_tyvar_headed = (h->kind == TY_TYVAR);
    }
    if (!recv_is_tyvar && !result_is_tyvar_headed) return NULL;
    const TypeClass *mtc = e->as.call_.dict_arg->as.dict_.instance->typeclass;
    for (uint8_t c = 0; c < cs->n_constraints; c++)
        if (cs->constraints[c].typeclass == mtc) return mtc;
    return NULL;
}

static void mapper_scan_dispatch(const Expr *e, const ConstraintSet *cs,
                                 const TypeClass **out_classes, uint8_t *n_out) {
    if (!e) return;
#define MS(x) mapper_scan_dispatch((x), cs, out_classes, n_out)
    switch (e->kind) {
        case EX_CALL: {
            const TypeClass *mtc = call_dispatched_constraint_class(e, cs);
            if (mtc) {
                bool seen = false;
                for (uint8_t k = 0; k < *n_out; k++)
                    if (out_classes[k] == mtc) { seen = true; break; }
                if (!seen && *n_out < MAX_FN_CONSTRAINTS)
                    out_classes[(*n_out)++] = mtc;
            }
            if (e->as.call_.fn_expr) MS(e->as.call_.fn_expr);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) MS(e->as.call_.args[i]);
            return;
        }
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++) MS(e->as.let_.bindings[i].init);
            MS(e->as.let_.body);
            return;
        case EX_IF: MS(e->as.if_.cond); MS(e->as.if_.then_); MS(e->as.if_.else_or_null); return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) MS(e->as.do_.items[i]);
            return;
        case EX_WHILE: MS(e->as.while_.cond); MS(e->as.while_.body); return;
        case EX_MATCH:
            MS(e->as.match_.scrutinee);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                MS(e->as.match_.arms[i].body); MS(e->as.match_.arms[i].guard);
            }
            return;
        case EX_ASCRIBE:      MS(e->as.ascribe_.inner); return;
        case EX_CAST:         MS(e->as.cast_.expr); return;
        case EX_REINTERPRET:  MS(e->as.reinterpret_.expr); return;
        /* Lambda boundaries -- do NOT descend: EX_VAR (a lifted lambda),
         * EX_FN/EX_FN_DEF (inline lambda), EX_POLY_WRAP / EX_CLOSURE (a boxed
         * mapper) are each their own scope, lowered by the recursive walk. */
        default: return;
    }
#undef MS
}

/* Rewrite a poly-wrap `pw` that boxes an already-converted dict-capturing mapper
 * `M` into the is-closure form so EX_CLOSURE emit builds the env (the
 * wrapper-binding path emits a NULL env).  Idempotent per pw.  Handles the case
 * where the SAME lifted mapper is boxed at more than one poly-wrap site: the
 * first call converts M; every poly-wrap over M (including later ones) is
 * rewritten here so none is left dangling at the pre-conversion arity. */
static void rewrite_poly_wrap_to_dict_closure(Elab *e, Expr *pw, FnDef *M,
                                              Span span) {
    if (!pw || pw->kind != EX_POLY_WRAP || pw->as.poly_wrap_.is_closure ||
        !M->closure)
        return;
    Type clo_ty = M->dict_env_mapper_ty;
    clo_ty.as.fn.boxed = true;
    Expr *cloexpr = expr_new(e->arena, EX_CLOSURE, clo_ty, span);
    cloexpr->as.closure_.closure = M->closure;
    /* The mapper's old captureless poly-wrapper is now dead (the EX_CLOSURE form
     * replaces it) and would emit a stale call to the pre-conversion mapper --
     * mark its FnDef skip-emission.  The wrapper binding has no source_fn_def
     * back-link (make_poly_wrapper_ex does not set one), so find its EX_FN_DEF
     * among the file defs by binding identity. */
    Binding *old_wrapper = pw->as.poly_wrap_.wrapper_binding;
    if (old_wrapper) {
        for (uint32_t d = 0; d < e->n_file_scope_defs; d++) {
            Expr *wd = e->file_scope_defs[d];
            if (wd && wd->kind == EX_FN_DEF && wd->as.fn_def_.fn &&
                wd->as.fn_def_.fn->binding == old_wrapper) {
                wd->as.fn_def_.fn->skip_emission = true;
                break;
            }
        }
    }
    pw->as.poly_wrap_.is_closure = true;
    pw->as.poly_wrap_.wrapper_binding = NULL;
    pw->as.poly_wrap_.inner = cloexpr;
}

/* Convert a captureless mapper lambda `M` (reached through a poly-wrap `pw`) into
 * a closure that CAPTURES one runtime dict per dispatched class, so its
 * class-method call dispatches through the env-loaded dict at emit
 * (emit_call_name).  Prepends the void* env param, builds the Closure with N dict
 * captures, marks the FnDef (dict_env_*), and rewrites `pw` into the is-closure
 * form so EX_CLOSURE emit builds the env (mirrors the capturing-mapper path
 * elab_fns.c already produces for a value capture).  Returns false (no-op) if M
 * is not the simple captureless shape. */
static bool convert_mapper_to_dict_closure(Elab *e, Expr *pw, FnDef *M,
                                           Binding **cap_dicts, uint8_t n_cap,
                                           Binding **disp_dicts,
                                           const TypeClass **disp_classes,
                                           uint8_t n_disp, Span span) {
    if (!pw || !M || !cap_dicts || n_cap == 0) return false;
    if (n_cap > MAX_FN_CONSTRAINTS || n_disp > MAX_FN_CONSTRAINTS) return false;
    if (M->dict_env_converted) return true;   /* already converted (idempotent) */
    if (M->closure) return false;             /* already a capturing closure */
    if ((uint32_t)M->n_params + 1 > MAX_FN_ARITY) return false;
    if (M->binding->type.kind != TY_FN) return false;

    /* The mapper's user-facing (pre-env) fn type becomes the closure value type. */
    Type mapper_ty = M->binding->type;

    /* Prepend a void* env parameter. */
    uint32_t new_np = (uint32_t)(M->n_params + 1);
    Binding **np = (Binding **)arena_alloc(e->arena, new_np * sizeof(Binding *));
    Type *npt = (Type *)arena_alloc(e->arena, new_np * sizeof(Type));
    char epn[40];
    snprintf(epn, sizeof(epn), "__env_p_%u", elab_fresh_id(e));
    Binding *envp = binding_new(e, symtab_intern(e->st, strslice(epn, (uint32_t)strlen(epn))),
                                TYPE_PTR_VOID, false, false, span);
    np[0] = envp; npt[0] = TYPE_PTR_VOID;
    for (uint32_t i = 0; i < M->n_params; i++) {
        np[i + 1] = M->params[i];
        npt[i + 1] = M->param_types ? M->param_types[i] : M->params[i]->type;
    }
    M->params = np; M->n_params = new_np; M->param_types = npt;

    /* Rebuild M's fn type with the void* env kind prepended.  Copy the whole
     * type by value (preserving result info and EVERY per-arg flag --
     * arg_flags carries linear/unique/affine/relevant/borrow/fat/poly_fn), then
     * build fresh per-arg arrays one longer with env in slot 0.  Fresh arrays
     * (not an in-place shift) because the out-of-line arg storage is shared with
     * mapper_ty by value; shifting in place would both corrupt the source and
     * overflow its `old_arity`-sized allocation.  type_fn() alone will not do --
     * it would zero the flags and drop, e.g., a ^poly_fn marker. */
    uint32_t old_arity = mapper_ty.as.fn.arity;
    Type new_ty = mapper_ty;
    new_ty.as.fn.arity = new_np;
    {
        uint8_t *nk = tur_fn_args_alloc(new_np);
        uint8_t *nf = tur_fn_args_alloc(new_np);
        nk[0] = TY_PTR_VOID;  /* env slot */
        nf[0] = 0;            /* env slot carries no per-arg markers */
        for (uint32_t i = 0; i < old_arity; i++) {
            nk[i + 1] = mapper_ty.as.fn.arg_kinds[i];
            nf[i + 1] = mapper_ty.as.fn.arg_flags[i];
        }
        new_ty.as.fn.arg_kinds = nk;
        new_ty.as.fn.arg_flags = nf;
    }
    if (mapper_ty.as.fn.arg_full_types) {
        Type **shifted = (Type **)arena_alloc(e->arena, new_np * sizeof(Type *));
        shifted[0] = NULL;
        for (uint32_t i = 0; i < old_arity; i++) shifted[i + 1] = mapper_ty.as.fn.arg_full_types[i];
        new_ty.as.fn.arg_full_types = shifted;
    }
    M->binding->type = new_ty;
    /* The lifted mapper's EX_FN_DEF (registered as a file def) carries its own
     * copy of the fn type, which emit_fn_def reads for the parameter signature.
     * Keep it in sync with the env-prepended type, or emit pairs the new params
     * with the old (arity-1) type list and produces a malformed signature. */
    for (uint32_t d = 0; d < e->n_file_scope_defs; d++) {
        Expr *fd_expr = e->file_scope_defs[d];
        if (fd_expr && fd_expr->kind == EX_FN_DEF &&
            fd_expr->as.fn_def_.fn == M) {
            fd_expr->type = new_ty;
            break;
        }
    }

    /* Build the Closure capturing every dict this mapper needs -- the classes it
     * dispatches itself, plus (Phase 3) any dict forwarded to a nested mapper it
     * constructs.  The dispatch (dict_env_*) vectors carry only the classes THIS
     * mapper dispatches; forwarded-only dicts ride the env for the inner
     * construction and are never dispatched here. */
    struct Closure *clo = (struct Closure *)arena_alloc(e->arena, sizeof(struct Closure));
    clo->fn = M;
    clo->fat_captures_borrowed = false;   /* arena mem is not zeroed */
    Binding **caps = (Binding **)arena_alloc(e->arena, n_cap * sizeof(Binding *));
    for (uint8_t k = 0; k < n_cap; k++) caps[k] = cap_dicts[k];
    clo->captures = caps;
    clo->n_captures = n_cap;
    char en[32];
    snprintf(en, sizeof(en), "__env_%u", elab_fresh_id(e));
    clo->env_name = symtab_intern(e->st, strslice(en, (uint32_t)strlen(en)));
    clo->is_shift_receiver = false;   /* arena mem is not zeroed */
    clo->is_effect_payload = false;
    clo->capture_drop_insts = NULL;   /* Model R #1b: no Drop resolution here */
    clo->capture_clone_insts = NULL;
    M->closure = clo;
    for (uint8_t k = 0; k < n_disp; k++) {
        M->dict_env_classes[k] = (TypeClass *)disp_classes[k];
        M->dict_env_bindings[k] = disp_dicts[k];
    }
    M->n_dict_env = n_disp;
    M->dict_env_converted = true;
    M->dict_env_mapper_ty = mapper_ty;

    rewrite_poly_wrap_to_dict_closure(e, pw, M, span);
    return true;
}

/* Peel ascribe/fn-to-fat wrappers to the lifted-mapper FnDef a poly-wrap boxes,
 * or NULL if the inner is not a captureless lifted lambda. */
static FnDef *poly_wrap_lifted_mapper(const Expr *pw) {
    if (!pw || pw->kind != EX_POLY_WRAP) return NULL;
    const Expr *in = pw->as.poly_wrap_.inner;
    while (in && (in->kind == EX_ASCRIBE || in->kind == EX_FN_TO_FAT))
        in = in->kind == EX_ASCRIBE ? in->as.ascribe_.inner : in->as.fn_to_fat_.inner;
    if (!in || in->kind != EX_VAR) return NULL;
    const Binding *vb = in->as.var.binding;
    if (vb && vb->is_lifted_lambda && vb->source_fn_def && vb->source_fn_def->body)
        return vb->source_fn_def;
    return NULL;
}

/* forall-dict-pass-nested-mapper-general-plan (Phase 2): peel ascribe/fn-to-fat
 * wrappers to the CLOSURE FnDef a poly-wrap boxes -- a mapper that already
 * captures a value (`set`/`over` shape) is an EX_CLOSURE, not an EX_VAR to a
 * lifted lambda -- or NULL if the inner is not a fat closure. */
static FnDef *poly_wrap_closure_mapper(const Expr *pw) {
    if (!pw || pw->kind != EX_POLY_WRAP) return NULL;
    const Expr *in = pw->as.poly_wrap_.inner;
    while (in && (in->kind == EX_ASCRIBE || in->kind == EX_FN_TO_FAT))
        in = in->kind == EX_ASCRIBE ? in->as.ascribe_.inner : in->as.fn_to_fat_.inner;
    if (!in || in->kind != EX_CLOSURE) return NULL;
    struct Closure *clo = in->as.closure_.closure;
    if (clo && clo->fn && clo->fn->body) return clo->fn;
    return NULL;
}

/* Scan a mapper body for the constraint class(es) it dispatches DIRECTLY on its
 * tyvar receiver, then resolve each to the clone's dict binding (constraint
 * order).  Fills `classes[]`/`dbs[]` and returns the count (>0), or 0 when the
 * mapper has no direct dispatch or a class is unresolvable. */
static uint8_t mapper_dispatch_dicts(const Expr *body, const ConstraintSet *cs,
                                     Binding **dparams,
                                     const TypeClass **classes, Binding **dbs) {
    uint8_t n_cls = 0;
    mapper_scan_dispatch(body, cs, classes, &n_cls);
    if (n_cls == 0) return 0;
    for (uint8_t k = 0; k < n_cls; k++) {
        Binding *db = NULL;
        for (uint8_t c = 0; c < cs->n_constraints; c++)
            if (cs->constraints[c].typeclass == classes[k]) { db = dparams[c]; break; }
        if (!db) return 0;
        dbs[k] = db;
    }
    return n_cls;
}

/* forall-dict-pass-nested-mapper-general-plan (Phase 2): convert a CAPTURING
 * mapper closure `M` (it already has an env param + env-build for a value
 * capture) into a dict-capturing closure by APPENDING one dict binding per
 * dispatched class to its existing `Closure.captures` (growing the env struct).
 * No poly-wrap rewrite / env-param prepend is needed -- the closure form is
 * already what EX_CLOSURE emit expects.  Idempotent (skip if already converted).
 * Returns false (no-op) if M is not a capturing closure. */
static bool convert_capturing_mapper_to_dict_closure(Elab *e, FnDef *M,
                                           Binding **cap_dicts, uint8_t n_cap,
                                           Binding **disp_dicts,
                                           const TypeClass **disp_classes,
                                           uint8_t n_disp, Span span) {
    (void)span;
    if (!M || !cap_dicts || n_cap == 0) return false;
    if (n_cap > MAX_FN_CONSTRAINTS || n_disp > MAX_FN_CONSTRAINTS) return false;
    if (M->dict_env_converted) return true;   /* already converted (idempotent) */
    if (!M->closure) return false;            /* not a capturing closure */
    struct Closure *clo = M->closure;
    uint32_t old_n = clo->n_captures;
    if (old_n + (uint32_t)n_cap > 255) return false;  /* n_captures is uint8_t */
    /* Append the dict bindings at the END so existing capture indices (and any
     * __TUR_CAP_N__ references in the body) stay valid. */
    Binding **caps = (Binding **)arena_alloc(e->arena,
        (old_n + n_cap) * sizeof(Binding *));
    for (uint32_t i = 0; i < old_n; i++) caps[i] = clo->captures[i];
    for (uint8_t k = 0; k < n_cap; k++) caps[old_n + k] = cap_dicts[k];
    clo->captures = caps;
    clo->n_captures = (uint8_t)(old_n + n_cap);
    for (uint8_t k = 0; k < n_disp; k++) {
        M->dict_env_classes[k] = (TypeClass *)disp_classes[k];
        M->dict_env_bindings[k] = disp_dicts[k];
    }
    M->n_dict_env = n_disp;
    M->dict_env_converted = true;
    return true;
}

/* Add binding `b` to a dedup set of dict bindings. */
static void dict_set_add(Binding **set, uint8_t *n, Binding *b) {
    if (!b) return;
    for (uint8_t i = 0; i < *n; i++) if (set[i] == b) return;
    if (*n < MAX_FN_CONSTRAINTS) set[(*n)++] = b;
}

/* Report the clone's dict params a converted mapper M captures, so an ENCLOSING
 * lambda that constructs M's closure at runtime forwards those same dicts inward
 * (Phase 3).  Works for a mapper converted at any depth: the captured dicts are
 * exactly the ones in M's closure that are dparams. */
static void collect_dparam_captures(const FnDef *M, Binding **dparams, uint8_t nc,
                                    Binding **out, uint8_t *n_out) {
    if (!M || !M->closure) return;
    for (uint8_t i = 0; i < M->closure->n_captures; i++)
        for (uint8_t c = 0; c < nc; c++)
            if (M->closure->captures[i] == dparams[c]) {
                dict_set_add(out, n_out, dparams[c]);
                break;
            }
}

/* forall-dict-pass-nested-mapper-general-plan: walk the dict-clone body (or a
 * mapper body) converting every nested mapper that dispatches a constraint
 * method into a dict-capturing closure.  Accumulates into `caps_out` the union
 * of clone dict params captured by the converted mappers, so an enclosing lambda
 * knows which dicts to forward (Phase 3).  `dparams` are the clone's
 * per-constraint dict bindings (constraint order). */
static void dict_clone_lower_nested_mappers(Elab *e, Expr *node,
                                            const ConstraintSet *cs,
                                            Binding **dparams, Span span,
                                            int depth, Binding **caps_out,
                                            uint8_t *n_caps_out);

/* forall-dict-pass-nested-lambda-direct-apply: an inner lambda that is DIRECTLY
 * APPLIED in a mapper body -- `((fn [y] (show y)) x)` -- is elaborated to a
 * captureless lifted lambda `__fn_N` that is either called by name or, more
 * commonly, let-bound to a temp and applied through it (`let [t __fn_N] (t x)`).
 * Its typeclass dispatch is stranded in `__fn_N`, which has no closure env to
 * carry the clone's dict, so the nested-mapper lowering below cannot reach it
 * and it trips the TUR-E0311 guard.  These helpers beta-reduce such an
 * application in place -- `(__fn_N x)` becomes `(let [y x] (show y))`, splicing
 * `__fn_N`'s body into the enclosing mapper where the existing depth-0 lowering
 * captures the dict.  Beta-reduction is semantically neutral here (single
 * application, each argument bound once) and idempotent (the rewritten EX_LET no
 * longer matches).  See
 * docs/archive/history/forall-dict-direct-applied-nested-lambda-dispatch.md. */
typedef struct { const Binding *v; FnDef *fd; } LiftAlias;

/* Resolve a callee binding to the captureless lifted-lambda FnDef it names --
 * directly (a lifted-lambda binding) or through a let alias recorded in
 * `al[0..n_al)`.  NULL if it is not such a lambda. */
static FnDef *resolve_lifted_binding(const Binding *vb, const LiftAlias *al, int n_al) {
    if (!vb) return NULL;
    if (vb->is_lifted_lambda && vb->source_fn_def && vb->source_fn_def->body)
        return vb->source_fn_def;
    for (int i = 0; i < n_al; i++)
        if (al[i].v == vb) return al[i].fd;
    return NULL;
}

/* Resolve a call's callee -- held in `fn_binding` (direct-to-named form) or
 * `fn_expr` (indirect, an EX_VAR after peeling ascribe/fn-to-fat) -- to a
 * captureless lifted-lambda FnDef, following let aliases. */
static FnDef *resolve_lifted_callee(const Binding *fn_binding, const Expr *fx,
                                    const LiftAlias *al, int n_al) {
    FnDef *fd = resolve_lifted_binding(fn_binding, al, n_al);
    if (fd) return fd;
    while (fx && (fx->kind == EX_ASCRIBE || fx->kind == EX_FN_TO_FAT))
        fx = fx->kind == EX_ASCRIBE ? fx->as.ascribe_.inner : fx->as.fn_to_fat_.inner;
    if (fx && fx->kind == EX_VAR)
        return resolve_lifted_binding(fx->as.var.binding, al, n_al);
    return NULL;
}

#define LIFT_ALIAS_MAX 64

static void flatten_applied_lifted(Elab *e, Expr *node, const ConstraintSet *cs,
                                   const LiftAlias *al, int n_al, int depth) {
    if (!node || depth > 64) return;
#define FAL(x) flatten_applied_lifted(e, (x), cs, al, n_al, depth + 1)
    switch (node->kind) {
        case EX_CALL: {
            FnDef *fd = resolve_lifted_callee(node->as.call_.fn_binding,
                                              node->as.call_.fn_expr, al, n_al);
            if (fd && fd->n_params == node->as.call_.n_args &&
                dict_clone_dispatch_in_nested_lambda(fd->body, cs, true)) {
                uint32_t n = node->as.call_.n_args;
                LetBinding *lbs = (LetBinding *)arena_alloc(e->arena,
                    (n ? n : 1) * sizeof(LetBinding));
                for (uint32_t i = 0; i < n; i++) {
                    lbs[i].binding = fd->params[i];
                    lbs[i].init    = node->as.call_.args[i];
                }
                Expr *body = fd->body;
                node->kind = EX_LET;
                node->as.let_.bindings = lbs;
                node->as.let_.n = n;
                node->as.let_.body = body;
                for (uint32_t i = 0; i < n; i++) FAL(lbs[i].init);
                FAL(body);
                return;
            }
            if (node->as.call_.fn_expr) FAL(node->as.call_.fn_expr);
            for (uint32_t i = 0; i < node->as.call_.n_args; i++) FAL(node->as.call_.args[i]);
            return;
        }
        case EX_LET:
        case EX_LETREC: {
            LiftAlias ext[LIFT_ALIAS_MAX];
            int n_ext = n_al < LIFT_ALIAS_MAX ? n_al : LIFT_ALIAS_MAX;
            for (int i = 0; i < n_ext; i++) ext[i] = al[i];
            for (uint32_t i = 0; i < node->as.let_.n; i++) {
                FAL(node->as.let_.bindings[i].init);
                FnDef *fd = resolve_lifted_callee(NULL, node->as.let_.bindings[i].init,
                                                  al, n_al);
                if (fd && node->as.let_.bindings[i].binding && n_ext < LIFT_ALIAS_MAX) {
                    ext[n_ext].v  = node->as.let_.bindings[i].binding;
                    ext[n_ext].fd = fd;
                    n_ext++;
                }
            }
            flatten_applied_lifted(e, node->as.let_.body, cs, ext, n_ext, depth + 1);
            return;
        }
        case EX_IF: FAL(node->as.if_.cond); FAL(node->as.if_.then_); FAL(node->as.if_.else_or_null); return;
        case EX_DO:
            for (uint32_t i = 0; i < node->as.do_.n; i++) FAL(node->as.do_.items[i]);
            return;
        case EX_WHILE: FAL(node->as.while_.cond); FAL(node->as.while_.body); return;
        case EX_MATCH:
            FAL(node->as.match_.scrutinee);
            for (uint32_t i = 0; i < node->as.match_.n_arms; i++) {
                FAL(node->as.match_.arms[i].body); FAL(node->as.match_.arms[i].guard);
            }
            return;
        case EX_ASCRIBE:     FAL(node->as.ascribe_.inner); return;
        case EX_CAST:        FAL(node->as.cast_.expr); return;
        case EX_REINTERPRET: FAL(node->as.reinterpret_.expr); return;
        /* Lambda boundaries (EX_FN, EX_FN_DEF, EX_POLY_WRAP, EX_CLOSURE, a
         * lifted EX_VAR not in call position) are their own mappers, lowered by
         * the recursive dict_clone_lower_nested_mappers walk -- do not descend. */
        default: return;
    }
#undef FAL
}

/* Lower a single mapper `M` reached through poly-wrap `node` (`capturing` selects
 * the capturing-closure vs captureless promotion path).  First recurses into M's
 * OWN body to lower any deeper nested mappers and collect the dicts they need
 * forwarded; unions those with M's own direct dispatches; then converts M to
 * CAPTURE the whole union while DISPATCHING only its own classes.  Reports M's
 * captured dparams up through caps_out so M's enclosing lambda forwards them. */
static void lower_one_mapper(Elab *e, Expr *node, FnDef *M, bool capturing,
                             const ConstraintSet *cs, Binding **dparams,
                             Span span, int depth, Binding **caps_out,
                             uint8_t *n_caps_out) {
    if (!M) return;
    uint8_t nc = cs->n_constraints;
    if (M->dict_env_converted) {
        /* Shared body already lowered by an earlier clone or a sibling box site:
         * a captureless mapper's second poly-wrap still needs the is-closure
         * rewrite; then just report its captures upward. */
        if (!capturing) rewrite_poly_wrap_to_dict_closure(e, node, M, span);
        collect_dparam_captures(M, dparams, nc, caps_out, n_caps_out);
        return;
    }
    /* 0. Beta-reduce any directly-applied captureless lifted lambda in M's body
     * so a typeclass dispatch stranded inside one (`((fn [y] (show y)) x)`)
     * surfaces at depth-0 where steps 1-3 can capture the dict, instead of
     * tripping the TUR-E0311 guard. */
    flatten_applied_lifted(e, M->body, cs, NULL, 0, 0);
    /* 1. Lower deeper mappers in M's body; collect the dicts they need forwarded. */
    Binding *fwd[MAX_FN_CONSTRAINTS];
    uint8_t n_fwd = 0;
    dict_clone_lower_nested_mappers(e, M->body, cs, dparams, span, depth + 1,
                                    fwd, &n_fwd);
    /* 2. M's own direct dispatches (constraint order). */
    const TypeClass *own_cls[MAX_FN_CONSTRAINTS];
    Binding *own_dbs[MAX_FN_CONSTRAINTS];
    uint8_t n_own = mapper_dispatch_dicts(M->body, cs, dparams, own_cls, own_dbs);
    /* 3. Capture set = union(forwarded, own).  M dispatches only `own`; the
     * forwarded-only dicts ride the env purely to construct the inner mapper. */
    Binding *caps[MAX_FN_CONSTRAINTS];
    uint8_t n_cap = 0;
    for (uint8_t i = 0; i < n_fwd; i++) dict_set_add(caps, &n_cap, fwd[i]);
    for (uint8_t i = 0; i < n_own; i++) dict_set_add(caps, &n_cap, own_dbs[i]);
    if (n_cap == 0) return;   /* not a dispatching/forwarding mapper */
    bool ok = capturing
        ? convert_capturing_mapper_to_dict_closure(e, M, caps, n_cap,
                                                   own_dbs, own_cls, n_own, span)
        : convert_mapper_to_dict_closure(e, node, M, caps, n_cap,
                                         own_dbs, own_cls, n_own, span);
    if (ok) collect_dparam_captures(M, dparams, nc, caps_out, n_caps_out);
}

static void dict_clone_lower_nested_mappers(Elab *e, Expr *node,
                                            const ConstraintSet *cs,
                                            Binding **dparams, Span span,
                                            int depth, Binding **caps_out,
                                            uint8_t *n_caps_out) {
    if (!node || depth > 64) return;
#define LW(x) dict_clone_lower_nested_mappers(e, (x), cs, dparams, span, \
                                              depth + 1, caps_out, n_caps_out)
    switch (node->kind) {
        case EX_POLY_WRAP: {
            /* A captureless lifted mapper (EX_VAR to a lifted lambda) or a
             * capturing mapper (a fat EX_CLOSURE): lower it, threading forwarded
             * dicts up through caps_out. */
            FnDef *M = poly_wrap_lifted_mapper(node);
            if (M) {
                lower_one_mapper(e, node, M, false, cs, dparams, span, depth,
                                 caps_out, n_caps_out);
            } else {
                FnDef *CM = poly_wrap_closure_mapper(node);
                if (CM)
                    lower_one_mapper(e, node, CM, true, cs, dparams, span, depth,
                                     caps_out, n_caps_out);
            }
            LW(node->as.poly_wrap_.inner);
            return;
        }
        case EX_CALL:
            if (node->as.call_.fn_expr) LW(node->as.call_.fn_expr);
            for (uint32_t i = 0; i < node->as.call_.n_args; i++) LW(node->as.call_.args[i]);
            return;
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < node->as.let_.n; i++) LW(node->as.let_.bindings[i].init);
            LW(node->as.let_.body);
            return;
        case EX_IF: LW(node->as.if_.cond); LW(node->as.if_.then_); LW(node->as.if_.else_or_null); return;
        case EX_DO:
            for (uint32_t i = 0; i < node->as.do_.n; i++) LW(node->as.do_.items[i]);
            return;
        case EX_WHILE: LW(node->as.while_.cond); LW(node->as.while_.body); return;
        case EX_MATCH:
            LW(node->as.match_.scrutinee);
            for (uint32_t i = 0; i < node->as.match_.n_arms; i++) {
                LW(node->as.match_.arms[i].body); LW(node->as.match_.arms[i].guard);
            }
            return;
        case EX_FN_TO_FAT:   LW(node->as.fn_to_fat_.inner); return;
        case EX_ASCRIBE:     LW(node->as.ascribe_.inner); return;
        case EX_CAST:        LW(node->as.cast_.expr); return;
        case EX_REINTERPRET: LW(node->as.reinterpret_.expr); return;
        default: return;
    }
#undef LW
}

/* hkt-generic-calls-generic: a constrained generic calling ANOTHER constrained
 * generic at its own abstract type constructor --
 *
 *   (defn add-two [^Monad M ^Applicative M] [m : (M int)] : (M int)
 *     (add-one (add-one m)))
 *
 * -- elaborates the inner call against `add-one` itself: its constraints pin
 * `add-two`'s abstract `M`, so no dictionary is known yet and the call keeps
 * the plain carrier base, whose method calls resolve to a representative
 * instance.  Worse, a dict clone shares its original's body, and cloning
 * `add-one` for any other caller converts its continuation into a
 * dict-capturing closure and retires the captureless wrapper -- which the plain
 * `add-one` still names, so the program failed to link.
 *
 * Gap B above forwards the enclosing dict for a SINGLE-constraint callee at
 * elaboration.  This covers the rest at clone time, when the caller's dict
 * params exist: a call in the clone body (not inside a nested lambda, which
 * has no env slot for them) whose every constraint is higher-kinded, pins a
 * type variable, and matches one of the caller's own constraints by class and
 * variable is redirected to the callee's dict clone with the caller's dict
 * params prepended.  The dict params are memoized on the original, so every
 * clone of the caller shares the rewrite and the body stays idempotent.  The
 * clone returns the carrier, so a higher-kinded result is ascribed back to the
 * call's own type, as Route B does. */
#define DCF_MAX_ACTIVE 16
static const FnDef *dcf_active[DCF_MAX_ACTIVE];
static int dcf_n_active;

static bool dcf_class_is_hkt(const TypeClass *tc) {
    if (!tc || !tc->type_param_kinds) return false;
    for (uint8_t k = 0; k < tc->n_type_params; k++)
        if (tc->type_param_kinds[k] != KIND_STAR) return true;
    return false;
}

static void dict_clone_forward_generic_calls(Elab *e, Expr *node,
                                             const ConstraintSet *cs,
                                             Binding **dparams,
                                             const FnDef *self_orig,
                                             Binding *self_clone, int depth) {
    if (!node || depth > 64) return;
#define FW(x) dict_clone_forward_generic_calls(e, (x), cs, dparams, self_orig, \
                                               self_clone, depth + 1)
    switch (node->kind) {
        case EX_CALL: {
            if (node->as.call_.fn_expr) FW(node->as.call_.fn_expr);
            for (uint32_t i = 0; i < node->as.call_.n_args; i++)
                FW(node->as.call_.args[i]);
            Binding *fb = node->as.call_.fn_binding;
            if (!fb || !fb->fn_constraints || node->as.call_.dict_arg ||
                !fb->source_fn_def || fb->source_fn_def->n_dict_clone > 0 ||
                !node->as.call_.abi_bindings)
                return;
            const ConstraintSet *ccs = fb->fn_constraints;
            uint8_t nc = ccs->n_constraints;
            if (nc < 1 || nc > MAX_FN_CONSTRAINTS) return;
            if ((uint32_t)node->as.call_.n_args + nc > MAX_FN_ARITY) return;
            Binding *fwd[MAX_FN_CONSTRAINTS];
            /* constrained-generic-relay-drops-dict: a kind-* constraint is
             * forwarded too, but RECORDED on the node rather than rewritten
             * (see below). */
            bool all_hkt = true;
            for (uint8_t ci = 0; ci < nc; ci++) {
                const TypeConstraint *con = &ccs->constraints[ci];
                if (!con->typeclass || !con->tyvar || !con->tyvar->name)
                    return;
                if (!dcf_class_is_hkt(con->typeclass)) all_hkt = false;
                const Type *bt = NULL;
                for (uint8_t bi = 0; bi < node->as.call_.n_abi_bindings; bi++) {
                    const AbiTypeBinding *ab = &node->as.call_.abi_bindings[bi];
                    if (ab->name && strcmp(ab->name, con->tyvar->name) == 0) {
                        bt = &ab->type;
                        break;
                    }
                }
                if (!bt) return;
                while (bt->kind == TY_APP && bt->as.app.fn) bt = bt->as.app.fn;
                if (bt->kind != TY_TYVAR || !bt->as.tyvar_.name) return;
                fwd[ci] = NULL;
                for (uint8_t k = 0; k < cs->n_constraints; k++) {
                    const TypeConstraint *own = &cs->constraints[k];
                    if (own->typeclass == con->typeclass && own->tyvar &&
                        own->tyvar->name &&
                        strcmp(own->tyvar->name, bt->as.tyvar_.name) == 0) {
                        fwd[ci] = dparams[k];
                        break;
                    }
                }
                if (!fwd[ci]) return;
            }
            Binding *clone = NULL;
            if (fb->source_fn_def == self_orig) {
                clone = self_clone;              /* direct recursion */
            } else {
                for (int a = 0; a < dcf_n_active; a++)
                    if (dcf_active[a] == fb->source_fn_def) return;  /* mutual */
                clone = make_dict_clone(e, fb, node->span);
            }
            if (!clone) return;
            if (!all_hkt) {
                /* constrained-generic-relay-drops-dict: `(defn ru [a] [(R a)]
                 * [v : a] : a (ri v))` -- ru's dict clone called ri's carrier
                 * BASE, whose `(rm x)` resolves to a representative instance,
                 * so a rank-2 `(use ru w)` ran the wrong instance on `w`.  The
                 * HKT rewrite below replaces the node, which every clone of
                 * the caller shares; a kind-* generic also has CONCRETE specs
                 * emitted from this body, where the dict params do not exist.
                 * So record the forward and let the emitter apply it only
                 * inside a dict clone that has these params. */
                Binding **fp = (Binding **)arena_alloc(e->arena,
                                                       nc * sizeof(Binding *));
                for (uint8_t ci = 0; ci < nc; ci++) fp[ci] = fwd[ci];
                node->as.call_.dict_fwd_clone = clone;
                node->as.call_.dict_fwd_params = fp;
                node->as.call_.dict_fwd_n = nc;
                return;
            }
            uint32_t na = node->as.call_.n_args;
            Expr **nargs = (Expr **)arena_alloc(e->arena,
                                                (na + nc) * sizeof(Expr *));
            for (uint8_t ci = 0; ci < nc; ci++) {
                Expr *dv = expr_new(e->arena, EX_VAR, fwd[ci]->type, node->span);
                dv->as.var.binding = fwd[ci];
                nargs[ci] = dv;
            }
            for (uint32_t k = 0; k < na; k++) nargs[nc + k] = node->as.call_.args[k];
            bool box_result = (node->type.kind == TY_APP);
            Expr *call = expr_new(e->arena, EX_CALL,
                                  box_result ? type_from_kind(TY_INT) : node->type,
                                  node->span);
            call->as.call_.fn_binding = clone;
            call->as.call_.args = nargs;
            call->as.call_.n_args = na + nc;
            call->as.call_.fn_expr = NULL;
            call->as.call_.abi_bindings = node->as.call_.abi_bindings;
            call->as.call_.n_abi_bindings = node->as.call_.n_abi_bindings;
            if (box_result) {
                memset(&node->as, 0, sizeof node->as);
                node->kind = EX_ASCRIBE;
                node->as.ascribe_.inner = call;
            } else {
                Type keep = node->type;
                *node = *call;
                node->type = keep;
            }
            return;
        }
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < node->as.let_.n; i++) FW(node->as.let_.bindings[i].init);
            FW(node->as.let_.body);
            return;
        case EX_IF: FW(node->as.if_.cond); FW(node->as.if_.then_); FW(node->as.if_.else_or_null); return;
        case EX_DO:
            for (uint32_t i = 0; i < node->as.do_.n; i++) FW(node->as.do_.items[i]);
            return;
        case EX_WHILE: FW(node->as.while_.cond); FW(node->as.while_.body); return;
        case EX_MATCH:
            FW(node->as.match_.scrutinee);
            for (uint32_t i = 0; i < node->as.match_.n_arms; i++) {
                FW(node->as.match_.arms[i].body); FW(node->as.match_.arms[i].guard);
            }
            return;
        case EX_ASCRIBE:     FW(node->as.ascribe_.inner); return;
        case EX_CAST:        FW(node->as.cast_.expr); return;
        case EX_REINTERPRET: FW(node->as.reinterpret_.expr); return;
        /* Lambda boundaries (EX_POLY_WRAP, EX_FN, EX_FN_DEF, EX_CLOSURE,
         * EX_FN_TO_FAT, an EX_VAR naming a lifted lambda): no env slot carries
         * the caller's dicts there, so leave those calls as they were. */
        default: return;
    }
#undef FW
}

/* MB1 / forall-dict-pass-multi-constraint-hkt-plan (Task 1.2): build a
 * dict-clone of a polymorphic constrained function `inner_b` (its body
 * dispatches class methods on its constrained type variables).  The clone shares
 * the original's body and trailing params but prepends one int64 dict param PER
 * constraint, IN CONSTRAINT ORDER (matching the call site's `mb1_dicts` prepend
 * order at elab_call.c so `poly_arg_mask << mb1_n_dicts` stays positionally
 * correct).  The FnDef is marked (dict_clone_*) so emit routes each class-method
 * call through the dict param for that method's own class.  Registers the clone
 * as a file def and returns its binding, or NULL if the original FnDef is not
 * found / arity would overflow. */
Binding *make_dict_clone(Elab *e, Binding *inner_b, Span span) {
    if (!inner_b || !inner_b->fn_constraints ||
        inner_b->fn_constraints->n_constraints < 1)
        return NULL;
    uint8_t nc = inner_b->fn_constraints->n_constraints;
    if (nc > MAX_FN_CONSTRAINTS) return NULL;
    if (inner_b->type.kind != TY_FN) return NULL;
    for (uint8_t c = 0; c < nc; c++)
        if (!inner_b->fn_constraints->constraints[c].typeclass) return NULL;
    /* The original FnDef, reached directly from the binding (MB1 link). */
    FnDef *orig = inner_b->source_fn_def;
    if (!orig || !orig->params || !orig->body) return NULL;
    uint32_t on = orig->n_params;
    if ((uint32_t)on + nc > MAX_FN_ARITY) return NULL;

    /* clone name */
    char cn[64];
    snprintf(cn, sizeof(cn), "%s__dict_%u",
             (inner_b->name && inner_b->name->name) ? inner_b->name->name : "fn",
             elab_fresh_id(e));
    const Symbol *csym = symtab_intern(e->st, strslice(cn, (uint32_t)strlen(cn)));

    uint8_t np = on + nc;
    Binding **params = (Binding **)arena_alloc(e->arena, np * sizeof(Binding *));
    Type *ptypes = (Type *)arena_alloc(e->arena, np * sizeof(Type));
    TypeKind akinds[MAX_FN_ARITY];
    /* One leading int64 dict param per constraint, in constraint order.  Phase 1
     * (forall-dict-pass-nested-lambda-dispatch-plan): memoize the dict param
     * bindings on the ORIGINAL FnDef so every clone of the same original reuses
     * the same binding identities/cnames -- a nested mapper lambda shared across
     * those clones can then capture one dict binding and read it from its env. */
    Binding *dparams[MAX_FN_CONSTRAINTS];
    bool reuse_memo = (orig->n_memo_dict == nc);
    for (uint8_t c = 0; c < nc; c++) {
        Binding *dparam;
        if (reuse_memo) {
            dparam = orig->memo_dict_params[c];
        } else {
            char dn[48];
            snprintf(dn, sizeof(dn), "__dict_%u", elab_fresh_id(e));
            const Symbol *dsym = symtab_intern(e->st, strslice(dn, (uint32_t)strlen(dn)));
            dparam = binding_new(e, dsym, type_from_kind(TY_INT), false, false, span);
        }
        dparams[c] = dparam;
        params[c] = dparam;
        ptypes[c] = type_from_kind(TY_INT);
        akinds[c] = TY_INT;
    }
    if (!reuse_memo) {
        for (uint8_t c = 0; c < nc; c++) orig->memo_dict_params[c] = dparams[c];
        orig->n_memo_dict = nc;
    }
    for (uint8_t i = 0; i < on; i++) {
        params[i + nc] = orig->params[i];
        ptypes[i + nc] = orig->param_types[i];
        akinds[i + nc] = (i < inner_b->type.as.fn.arity)
            ? inner_b->type.as.fn.arg_kinds[i] : orig->param_types[i].kind;
        /* MB4 (constrained-hkt-forall-mode-b-plan): a function-typed parameter
         * (a van Laarhoven lens's `g : (-> A (f A))`) crosses the poly carrier
         * as a uniform fat box.  Mark it `boxed` so the clone body fat-dispatches
         * it through slot 0 instead of calling the box as a thin function pointer
         * (a jump into the closure env -> SIGSEGV when the caller passes a
         * capturing closure, as `set`/`over` do).  The pass site boxes a thin fn
         * argument to match (elab_poly_call, EX_FN_TO_FAT above).  The body shares
         * `orig`'s param bindings, so mark the binding's own type -- `orig` is
         * only ever emitted as this dict-clone, never directly. */
        if (ptypes[i + nc].kind == TY_FN && !ptypes[i + nc].as.fn.boxed) {
            ptypes[i + nc].as.fn.boxed = true;
            if (params[i + nc]) params[i + nc]->type.as.fn.boxed = true;
        }
    }
    TypeKind rk = inner_b->type.as.fn.result_kind;
    Type ftype = type_fn(akinds, np, rk);
    /* MB2: the dict-clone dispatches through the carrier and returns the int64
     * carrier -- do NOT copy a result_full_type that mentions the (higher-kinded)
     * type variable (e.g. `(f int)`), or the wrapper that boxes this clone is
     * flagged generic-unsafe and skipped at emit.  A concrete-typed result is
     * left to the aggregate-functor (M7) path, out of MB2's scope. */
    if (inner_b->type.as.fn.result_full_type &&
        !type_mentions_tyvar(inner_b->type.as.fn.result_full_type))
        ftype.as.fn.result_full_type = inner_b->type.as.fn.result_full_type;

    Binding *cb = binding_new(e, csym, ftype, false, true, span);
    scope_add(&e->global, cb);
    FnDef *cf = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    memset(cf, 0, sizeof(FnDef));
    cf->binding = cb;
    cf->params = params;
    cf->n_params = np;
    cf->param_types = ptypes;
    cf->return_type = orig->return_type;
    cf->body = orig->body;
    for (uint8_t c = 0; c < nc; c++) {
        cf->dict_clone_params[c] = dparams[c];
        cf->dict_clone_classes[c] = inner_b->fn_constraints->constraints[c].typeclass;
    }
    cf->n_dict_clone = nc;
    constraint_set_init(&cf->constraints);
    /* Back-link the clone binding to its FnDef so a consumer (make_poly_wrapper_ex)
     * can see this is a dict-clone.  A dict-clone is ALWAYS emitted returning the
     * int64 carrier (emit_fns.c forces `int64_t` for any dict_clone_class body,
     * boxing an aggregate `(f a)` result and casting a scalar/pointer method
     * result through int64), so its poly wrapper must carry int64 too rather than
     * the method's declared C return type. */
    cb->source_fn_def = cf;
    /* forall-dict-pass-nested-lambda-dispatch-plan (Phase 2) +
     * forall-dict-pass-nested-mapper-general-plan (Phases 1-3): convert every
     * nested mapper that dispatches a constraint method into a dict-capturing
     * closure, so its method call dispatches through the env-loaded dict
     * (emit_call_name) instead of the baked representative.  Handles N dicts per
     * mapper (Phase 1), capturing mappers (Phase 2), and deeper nesting by
     * forwarding each dict through every enclosing lambda's env (Phase 3).  The
     * body is shared across clones of the same original, so the conversion is
     * idempotent (`dict_env_converted`) and each clone's env-build reuses the
     * memoized dict binding declared as that clone's leading param.  A dispatch
     * this walk cannot reach (e.g. inside a directly-applied lifted lambda) is
     * left untouched and caught by the TUR-E0311 guard at the pass site.  The
     * top-level clone body has each dict as a real PARAM, so its own returned
     * capture set is discarded. */
    Binding *top_caps[MAX_FN_CONSTRAINTS];
    uint8_t n_top_caps = 0;
    dict_clone_lower_nested_mappers(e, cf->body, inner_b->fn_constraints,
                                    dparams, span, 0, top_caps, &n_top_caps);
    /* hkt-generic-calls-generic: forward this clone's dicts into a nested
     * call to another constrained generic (see the walk's comment). */
    if (dcf_n_active < DCF_MAX_ACTIVE) {
        dcf_active[dcf_n_active++] = orig;
        dict_clone_forward_generic_calls(e, cf->body, inner_b->fn_constraints,
                                         dparams, orig, cb, 0);
        dcf_n_active--;
    }
    /* forall-dict-pass-nested-mapper-general-plan (Phase 4): Phases 1-3 lower the
     * reachable nested-mapper shapes -- N classes per mapper, capturing mappers,
     * and deeper nesting where each intermediate lambda forwards the dict through
     * its env.  The one residual the walk cannot reach is a dispatch inside a
     * lifted lambda that is DIRECTLY APPLIED in place (`((fn [y] (show y)) x)`):
     * that lambda is lifted and called by name, with no closure env to thread a
     * captured dict through.  Keep this as a defensive guard for that shape --
     * inside make_dict_clone so BOTH the rank-2 pass site and the lens-
     * composition (Gap B) site are covered -- rather than silently mis-resolving
     * to the carrier-representative instance.  Dispatching a constraint method
     * directly in the body (e.g. `(let [tag (show s)] ...)`), or passing the
     * inner lambda as a value to another call, are always supported. */
    if (dict_clone_dispatch_in_nested_lambda(cf->body, inner_b->fn_constraints,
                                             false)) {
        diag_emit(DIAG_ERROR, span,
            "forall-dict-pass: '%s' dispatches a typeclass method on its "
            "constrained type variable from inside a directly-applied nested "
            "lambda that cannot be lowered (a lifted lambda called by name, with "
            "no closure env to carry the dict). Bind the method directly in the "
            "function body with a `let`, or pass the inner lambda as a value to "
            "another call, instead (TUR-E0311)",
            inner_b->name ? inner_b->name->name : "?");
        return NULL;
    }
    Expr *cdef = expr_new(e->arena, EX_FN_DEF, ftype, span);
    cdef->as.fn_def_.fn = cf;
    elab_register_file_def(e, cdef);
    return cb;
}

/* Phase HRT1: create a poly wrapper thunk for passing a function to a rank-2 param.
 * The wrapper has signature: int64_t __poly_N(void *env, int64_t x0, ..., int64_t x_{arity-1})
 * Its body calls inner_b(x0, ..., x_{arity-1}), ignoring env.
 * Registers the wrapper as a file-level EX_FN_DEF and returns the wrapper Binding. */
Binding *make_poly_wrapper(Elab *e, Binding *inner_b, uint8_t inner_arity, Span span, bool typed_concrete) {
    return make_poly_wrapper_ex(e, inner_b, inner_arity, 0, span, typed_concrete);
}

/* MB1 (constrained-hkt-forall-mode-b-plan): `n_lead_ignore` leading int64
 * carrier params (dictionary pointers) sit between the env slot and the
 * forwarded args and are NOT passed to the inner fn -- the carrier ABI of a
 * constrained rank-2 forall always carries one dict per constraint, but a
 * *monomorphic* inner ignores them (a genuinely polymorphic inner is wrapped as
 * a dict-clone whose dict param IS one of the forwarded args, so it uses
 * n_lead_ignore = 0). */
Binding *make_poly_wrapper_ex(Elab *e, Binding *inner_b, uint8_t inner_arity,
                              uint8_t n_lead_ignore, Span span,
                              bool typed_concrete) {
    /* Wrapper name */
    char wname[32];
    snprintf(wname, sizeof(wname), "__poly_%u", elab_fresh_id(e));
    const Symbol *wsym = symtab_intern(e->st, strslice(wname, (uint32_t)strlen(wname)));

    /* Wrapper params: env (ptr<void>) + n_lead_ignore dict slots + inner_arity args */
    uint8_t w_arity = inner_arity + n_lead_ignore + 1;
    if (w_arity > MAX_FN_ARITY) {
        diag_emit(DIAG_ERROR, span, "rank-2 wrapper: too many arguments");
        return NULL;
    }
    Binding **wparams = (Binding **)arena_alloc(e->arena, w_arity * sizeof(Binding *));
    Type *wparam_types = (Type *)arena_alloc(e->arena, w_arity * sizeof(Type));

    /* env param */
    char env_pname[40];
    snprintf(env_pname, sizeof(env_pname), "__poly_env_%u", elab_fresh_id(e));
    const Symbol *env_psym = symtab_intern(e->st, strslice(env_pname, (uint32_t)strlen(env_pname)));
    Binding *env_pb = binding_new(e, env_psym, TYPE_PTR_VOID, false, false, span);
    wparams[0] = env_pb;
    wparam_types[0] = TYPE_PTR_VOID;

    /* MB1: ignored leading dict slots (int64 carriers), never forwarded. */
    for (uint8_t j = 0; j < n_lead_ignore; j++) {
        char dpn[44];
        snprintf(dpn, sizeof(dpn), "__poly_dictskip%u_%u", j, elab_fresh_id(e));
        const Symbol *ds = symtab_intern(e->st, strslice(dpn, (uint32_t)strlen(dpn)));
        Binding *dpb = binding_new(e, ds, type_from_kind(TY_INT), false, false, span);
        wparams[1 + j] = dpb;
        wparam_types[1 + j] = type_from_kind(TY_INT);
    }
    uint8_t argbase = 1 + n_lead_ignore;  /* first forwarded-arg param index */

    /* poly-wrapper-forces-int64-args-non-int-fat-sink.md: by default the
     * wrapper carries each argument as the int64_t carrier, which is correct
     * for every int64-register-class kind (int/ptr/cstr/bool).  A *float*-class
     * argument lives in a different register class (xmm0), so an int64-typed
     * wrapper param would read it from the wrong register -- and the carrier's
     * stored thunk would then mismatch the typed slot-0 poly-to-fat shim that a
     * non-int64 ^fat sink invokes.  Retype only float-class args of a *plain*
     * named inner fn (a closure inner_b stores its real thunk through the
     * is_closure pass-through and never reaches this wrapper), keeping
     * int64-register kinds churn-free. */
    bool inner_is_plain_fn = (inner_b->type.kind == TY_FN &&
                              inner_b->closure_fn_binding == NULL);
    TypeKind real_arg_kinds[MAX_FN_ARITY];
    for (uint32_t i = 0; i < inner_arity; i++) {
        TypeKind rk = TY_INT;
        if (inner_is_plain_fn && i < inner_b->type.as.fn.arity) {
            TypeKind k = inner_b->type.as.fn.arg_kinds[i];
            const Type *aft = inner_b->type.as.fn.arg_full_types
                ? inner_b->type.as.fn.arg_full_types[i] : NULL;
            bool is_poly = aft && aft->kind == TY_FORALL;
            bool is_float = (k == TY_FLOAT || k == TY_FLOAT32 || k == TY_FLOAT64);
            /* poly-fn-with-any-parameter-is-called-with-the-int64-carrier: an
             * `any` is the float case again, one size up.  It is a 16-byte
             * `tur_tagged_t`, so it cannot ride the int64 carrier at all -- and
             * the CALL side already casts `g.fn` to the typed signature
             * `(tur_tagged_t (*)(void *, tur_tagged_t))` -- so an int64-typed
             * wrapper param was a hard C error at the inner call ("expected
             * 'tur_tagged_t' but argument is of type 'int64_t'"), which is
             * what took `fmap` over an `(Option any)` off the table entirely.
             * Retype it like float; the wrapper's RESULT was already the
             * declared kind, so only the parameter side was wrong. */
            bool is_any = (k == TY_ANY);
            /* F5: for a *typed* `:fn` carrier the call site casts fn.fn to the
             * concrete signature, so the wrapper must accept each argument in its
             * native kind (cstr/ptr/sub-int/float) -- not the int64 carrier --
             * otherwise the wrapper would truncate the value through int64 (a
             * -Wint-conversion at the inner call, "works by luck" for pointers).
             * For the bare carrier we keep the int64 default (only float, which
             * the bare carrier rejects, is retyped). */
            if (!is_poly && (is_float || is_any || typed_concrete)) rk = k;
        }
        real_arg_kinds[i] = rk;
    }

    /* Arg params x0, x1, ... */
    Binding *arg_bs[MAX_FN_ARITY];
    for (uint32_t i = 0; i < inner_arity; i++) {
        char apname[40];
        snprintf(apname, sizeof(apname), "__poly_x%u_%u", i, elab_fresh_id(e));
        const Symbol *apsym = symtab_intern(e->st, strslice(apname, (uint32_t)strlen(apname)));
        Type apt = type_from_kind(real_arg_kinds[i]);
        Binding *apb = binding_new(e, apsym, apt, false, false, span);
        wparams[argbase + i] = apb;
        wparam_types[argbase + i] = apt;
        arg_bs[i] = apb;
    }

    /* MB2 (constrained-hkt-forall-mode-b-plan): a dict-clone is emitted returning
     * the int64 carrier for EVERY method result (emit_fns.c forces `int64_t` off
     * dict_clone_class), so the wrapper that boxes it must return int64 as well --
     * deriving the result kind from the method's declared type (e.g. `cstr` for
     * `Show::show`) would make the wrapper body `return dict_clone(...)` an
     * int64->pointer conversion (the Deficit-1 -Wint-conversion error).  The poly
     * carrier's caller already casts the int64 result back to the declared type. */
    bool inner_is_dict_clone = inner_b->source_fn_def &&
        inner_b->source_fn_def->n_dict_clone > 0;

    /* Build call body: (inner_b x0 x1 ...) */
    TypeKind inner_result_kind = inner_is_dict_clone ? TY_INT
        : ((inner_b->type.kind == TY_FN)
            ? inner_b->type.as.fn.result_kind : TY_INT);
    Expr **call_args = (Expr **)arena_alloc(e->arena, (inner_arity ? inner_arity : 1) * sizeof(Expr *));
    uint64_t call_poly_mask = 0;
    uint64_t call_agg_mask = 0;
    for (uint32_t i = 0; i < inner_arity; i++) {
        Expr *av = expr_new(e->arena, EX_VAR, type_from_kind(real_arg_kinds[i]), span);
        av->as.var.binding = arg_bs[i];
        call_args[i] = av;
        /* Phase HRT3: if inner_b's param i is a poly fn, the wrapper receives it as int64_t
         * (a pointer to a stack-allocated tur_poly_fn_t). Mark it so emit can dereference. */
        if (inner_b->type.kind == TY_FN && inner_b->type.as.fn.arg_full_types) {
            const Type *aft = inner_b->type.as.fn.arg_full_types[i];
            if (aft && aft->kind == TY_FORALL) {
                call_poly_mask |= ARG_IDX_BIT(i);
            }
            /* Slice 3 (constrained-hkt-forall codegen): a by-value aggregate
             * param `(f a)` = `(Option int)` arrives through the carrier as an
             * int64 heap-box pointer; mark it so the inner call derefs it back
             * to the aggregate.  `av` stays int64 (the carrier value the wrapper
             * param holds); the emit unbox reads the target C type from the
             * callee's own parameter full type. */
            else if (aft &&
                     ((aft->kind == TY_APP && adt_app_is_byvalue_product(*aft)) ||
                      (aft->kind == TY_ADT && aft->as.adt_.def &&
                       !aft->as.adt_.def->is_heap &&
                       adt_is_byvalue_product(aft->as.adt_.def)))) {
                call_agg_mask |= ARG_IDX_BIT(i);
            }
        }
    }
    Expr *call_body = expr_new(e->arena, EX_CALL, type_from_kind(inner_result_kind), span);
    call_body->as.call_.fn_binding = inner_b;
    call_body->as.call_.args = inner_arity > 0 ? call_args : NULL;
    call_body->as.call_.n_args = inner_arity;
    call_body->as.call_.fn_expr = NULL;
    call_body->as.call_.dict_arg = NULL;
    call_body->as.call_.is_poly_call = false;
    call_body->as.call_.poly_arg_mask = call_poly_mask;
    call_body->as.call_.poly_agg_arg_mask = call_agg_mask;

    /* Build wrapper fn type */
    TypeKind warg_kinds[MAX_FN_ARITY];
    warg_kinds[0] = TY_PTR_VOID;
    for (uint8_t j = 0; j < n_lead_ignore; j++) warg_kinds[1 + j] = TY_INT;
    for (uint32_t i = 0; i < inner_arity; i++) warg_kinds[argbase + i] = real_arg_kinds[i];
    Type wfn_type = type_fn(warg_kinds, w_arity, inner_result_kind);
    /* M7: when the inner fn returns a by-value aggregate (a Monad/HKT
     * continuation returning `(m b)` -> e.g. `Option__int`), carry its FULL
     * result type on the wrapper so the wrapper body `return inner(x)` is a
     * struct->struct return (valid C) rather than struct->int64 (a hard error).
     * The pack site (EX_POLY_WRAP) then routes the struct-returning wrapper
     * through a carrier-spill shim to satisfy the int64 tur_poly_fn_t.fn ABI. */
    if (!inner_is_dict_clone &&
        inner_b->type.kind == TY_FN && inner_b->type.as.fn.result_full_type) {
        const Type *irf = inner_b->type.as.fn.result_full_type;
        /* fmap-over-byvalue-struct-element-passes-the-struct-as-int64: a plain
         * by-value `defstruct` result (TY_ADT, e.g. `Pt`) belongs to the same
         * convention as the parametric monomorph -- the typed-carrier call site
         * casts `g.fn` to `(tur_adt_Pt (*)(void *, ...))` and reads the struct
         * BY VALUE, and a lambda's typed thunk returns it that way.  Left as
         * the int64 carrier, the wrapper took the fat-return path and
         * malloc-boxed the struct, which that cast then read as the struct
         * itself.  Same shape test as the by-value PARAM arm above. */
        bool aggr = irf->kind == TY_APP ||
                    (irf->kind == TY_ADT && irf->as.adt_.def &&
                     !irf->as.adt_.def->is_heap &&
                     adt_is_byvalue_product(irf->as.adt_.def));
        if (aggr) {
            Type *rft = (Type *)arena_alloc(e->arena, sizeof(Type));
            *rft = *irf;
            wfn_type.as.fn.result_full_type = rft;
        }
    }

    Binding *wb = binding_new(e, wsym, wfn_type, false, true, span);
    /* `__poly_N` forwards to `inner_b`; it has no source form and nobody can
     * annotate it, so diagnostics describe it by what it wraps (and the
     * --strict-effects lint skips it -- `inner_b` answers for its own row). */
    wb->is_synthesized = true;
    wb->synth_kind = SYNTH_FORWARDING_WRAPPER;
    {
        char lbl[160];
        int n = snprintf(lbl, sizeof(lbl), "rank-2 wrapper for '%s'",
                         inner_b && inner_b->name ? inner_b->name->name : "?");
        if (n > 0 && (size_t)n < sizeof(lbl))
            wb->diag_label = arena_strdup(e->arena, lbl, (size_t)n);
    }
    scope_add(&e->global, wb);

    FnDef *wfd = (FnDef *)arena_alloc(e->arena, sizeof(FnDef));
    memset(wfd, 0, sizeof(FnDef));
    wfd->binding = wb;
    wfd->params = wparams;
    wfd->n_params = w_arity;
    wfd->body = call_body;
    wfd->is_variadic = false;
    wfd->closure = NULL;
    wfd->inferred_effect_row = NULL;
    wfd->param_types = wparam_types;
    constraint_set_init(&wfd->constraints);

    Expr *wdef = expr_new(e->arena, EX_FN_DEF, wfn_type, span);
    wdef->as.fn_def_.fn = wfd;
    elab_register_file_def(e, wdef);

    return wb;
}

/* Phase HRT1/HRT4: Helper to extract the underlying fn binding from a poly arg expression.
 * Handles EX_VAR and EX_ASCRIBE(EX_VAR). Returns NULL if not a simple fn ref.
 * Phase HRT4: follows source_binding for let-bound aliases of global functions. */
Binding *poly_arg_fn_binding(Expr *arg) {
    if (arg->kind == EX_VAR) {
        Binding *b = arg->as.var.binding;
        /* For is_poly_fn bindings (already tur_poly_fn_t), return as-is — caller uses passthrough. */
        if (b->is_poly_fn) return b;
        /* Follow source_binding chain to resolve let-bound aliases back to global fns. */
        if (b->source_binding) return b->source_binding;
        return b;
    }
    if (arg->kind == EX_ASCRIBE) return poly_arg_fn_binding(arg->as.ascribe_.inner);
    return NULL;
}

/* Phase HRT1: Elaborate a call through a rank-2 polymorphic function parameter.
 * fn_binding->is_poly_fn is true; the call emits fn_name.fn(fn_name.env, args...) */
static Expr *elab_poly_call(Elab *e, const Form *call, Binding *fn_binding) {
    uint32_t n_args = call->as.list.len - 1;

    /* turi-dict-passing-plan probe (2026-08-16): snapshot the expected-type
     * channel for THIS call (pushed by an ascription or typed let) for the
     * MB1 dict pin and the result instantiation below.  Deliberately NOT
     * cleared or restored: every fn-value invocation routes through this
     * function (`(pred c)` on a plain fn param included), and perturbing the
     * channel here starves sibling ctor inference in the enclosing body --
     * measured as a parsec-tutorial if-branch mismatch on the first cut. */
    Type *poly_expected = e->expected_type;

    /* Elaborate all arguments normally */
    Expr **args = (Expr **)arena_alloc(e->arena, (n_args ? n_args : 1) * sizeof(Expr *));
    for (uint32_t i = 0; i < n_args; i++) {
        args[i] = elab_form(e, call->as.list.items[1 + i]);
        if (!args[i]) return NULL;
    }

    /* Phase CCL (fn-first-class-application): a bare `:fn` value (mono
     * poly-closure carrier, poly_type == NULL) round-trips through the int64
     * carrier.  Integer-register kinds (int/cstr/ptr/bool) survive, but a
     * float-class argument lives in a different register class (xmm0), so passing
     * it through the int64 carrier is a silent register-class miscompile.  Reject
     * it with a hard error rather than miscompile -- a typed `:fn` signature
     * (the F5 phase of the plan) is the proper fix.  See
     * docs/archive/history/fn-first-class-float-carrier-gap.md. */
    if (fn_binding->poly_type == NULL) {
        for (uint32_t i = 0; i < n_args; i++) {
            if (kind_is_non_int_register_class(args[i]->type.kind) ||
                args[i]->type.kind == TY_FLOAT32 || args[i]->type.kind == TY_FLOAT64) {
                diag_emit(DIAG_ERROR, args[i]->span,
                          "applying a `:fn` value to a floating-point argument is "
                          "not supported: the first-class `:fn` carrier is the "
                          "int64 register class, so a float argument would be a "
                          "silent register-class miscompile; pass the value through "
                          "an int-carried wrapper, or use a typed function "
                          "parameter (e.g. `g : (fn [float] : float)`) instead");
                return NULL;
            }
        }
    }

    /* Phase HRT3: Detect nested poly-fn args in the body type.
     * If body->arg_full_types[i] is TY_FORALL, wrap that arg with EX_POLY_WRAP
     * and mark it in poly_arg_mask so emit can pass it by pointer. */
    const Type *poly = fn_binding->poly_type;

    /* fn-param-call-prototype-spelled-from-the-call-not-the-fn (argument
     * half): an F5 typed carrier whose parameter is `any` -- `g : (fn [any
     * any] any)`, an instance method's unannotated `g` typed from its class
     * -- holds a thunk that takes `tur_tagged_t`, so a concrete argument
     * must be widened exactly as the direct-call path widens one (IT4).
     * Without it the emitter spelled the slot from the argument (`double`)
     * and the callee read a raw double as a tagged word: `cast: any holds
     * unknown`. */
    if (poly && poly->kind == TY_FN) {
        for (uint32_t i = 0; i < n_args && i < (uint32_t)poly->as.fn.arity; i++) {
            TypeKind pk = (poly->as.fn.arg_full_types && poly->as.fn.arg_full_types[i])
                              ? poly->as.fn.arg_full_types[i]->kind
                              : (TypeKind)poly->as.fn.arg_kinds[i];
            if (pk == TY_ANY && args[i] && args[i]->type.kind != TY_ANY)
                args[i] = elab_coerce_to_any(e, args[i]);
        }
    }

    /* Slice 3 (constrained-hkt-forall): gate + validate a higher-kinded rank-2
     * invocation.  When the callee applies a poly fn whose forall quantifies an
     * `f :: * -> *` used as `(f a)`, the actual argument at that position must be
     * a type application whose base constructor kind matches f's kind. */
    if (poly && poly->kind == TY_FORALL &&
        forall_has_higher_kinded_var(poly)) {
        const Type *hbody = poly->as.forall_.body;
        if (hbody && hbody->kind == TY_FN && hbody->as.fn.arg_full_types) {
            for (uint8_t bp = 0;
                 bp < hbody->as.fn.arity && bp < n_args; bp++) {
                Kind fk;
                if (!hrt_body_param_hk_var_kind(poly,
                        hbody->as.fn.arg_full_types[bp], &fk))
                    continue;
                if (!hrt_validate_hk_actual(e, fk, args[bp]->type,
                                            args[bp]->span, "rank-2 call"))
                    return NULL;
            }
        }
    }

    uint64_t poly_arg_mask = 0;
    if (poly && poly->kind == TY_FORALL) {
        const Type *pbody = poly->as.forall_.body;
        if (pbody && pbody->kind == TY_FN && pbody->as.fn.arg_full_types) {
            for (uint32_t i = 0; i < n_args && i < (uint32_t)pbody->as.fn.arity; i++) {
                const Type *aft = pbody->as.fn.arg_full_types[i];
                if (aft && aft->kind == TY_FORALL) {
                    /* Arg i is a nested poly fn — wrap it or pass through if already poly fn. */
                    Binding *inner_b = poly_arg_fn_binding(args[i]);
                    if (!inner_b) {
                        diag_emit(DIAG_ERROR, call->as.list.items[1 + i]->span,
                                  "rank-3: polymorphic function argument must be a named function");
                        return NULL;
                    }
                    Expr *orig_arg = args[i];
                    Expr *wrap = expr_new(e->arena, EX_POLY_WRAP, TYPE_PTR_VOID, orig_arg->span);
                    wrap->as.poly_wrap_.inner = orig_arg;
                    wrap->as.poly_wrap_.boxes_aggregate = true;  /* Slice 3: forall carrier */
                    if (inner_b->is_poly_fn) {
                        /* HRT4: pass-through — already a tur_poly_fn_t. */
                        wrap->as.poly_wrap_.wrapper_binding = NULL;
                    } else {
                        uint32_t inner_arity = (inner_b->type.kind == TY_FN)
                            ? (uint8_t)inner_b->type.as.fn.arity : 1;
                        Binding *wrapper_b = make_poly_wrapper(e, inner_b, inner_arity, args[i]->span, false);
                        if (!wrapper_b) return NULL;
                        wrap->as.poly_wrap_.wrapper_binding = wrapper_b;
                    }
                    args[i] = wrap;
                    poly_arg_mask |= ARG_IDX_BIT(i);
                    continue;
                }
                /* fn-value-fat-normalization increment 2: a fn-typed slot in
                 * the forall body is fat-normalized like any other nominal fn
                 * param, so a bare/thin fn argument must be shimmed HERE too.
                 * This call goes through the carrier (`l.fn(l.env, arg)`), so
                 * the A#1 call-site shim never sees it -- that gap is what made
                 * the tyvar rows of the poly-result crash table survive stage 1
                 * ("a tyvar-sig param's arguments arrive through the
                 * generic/carrier machinery as thin pointers the call-site shim
                 * cannot see").  Every function reachable through this forall
                 * declares the same slot type, so both ends agree by type.
                 * Already-fat values (boxed TY_FN, TY_PTR_VOID) pass through. */
                if (aft && aft->kind == TY_FN &&
                    fn_param_type_is_fat_normalized(aft) &&
                    args[i]->type.kind == TY_FN && !args[i]->type.as.fn.boxed &&
                    args[i]->type.as.fn.arity >= 1 &&
                    args[i]->type.as.fn.arity <= 5) {
                    Type *bt = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *bt = args[i]->type;
                    bt->as.fn.boxed = true;
                    Expr *shim = expr_new(e->arena, EX_FN_TO_FAT, *bt,
                                          args[i]->span);
                    shim->as.fn_to_fat_.inner = args[i];
                    args[i] = shim;
                }
            }
        }
    }

    /* Slice 2 (constrained-hkt-forall-plan): re-discharge any typeclass
     * constraints carried on the rank-2 forall at THIS instantiation site.
     * Turmeric's HRT is type-erased -- the poly fn is a monomorphic function
     * passed through the int64 carrier -- so no runtime dictionary is threaded;
     * the constraint's teeth are a static check that the concrete type filling
     * each constrained bound variable has an in-scope instance.  Mirrors the
     * exists/pack enforcement (elab_types.c) and the defn-constraint re-discharge
     * (elab_call.c ~4951).  A bound var pinned only through the return context
     * (not by any argument) is left abstract here and discharged by the caller's
     * own context. */
    /* MB1: dictionaries resolved for each constraint, to prepend as leading
     * carrier args (in constraint order) when forall-dict-pass is enabled. */
    Expr *mb1_dicts[16];
    uint8_t mb1_n_dicts = 0;
    if (poly && poly->kind == TY_FORALL && poly->as.forall_.n_constraints > 0) {
        const Type *cbody = poly->as.forall_.body;
        if (cbody && cbody->kind == TY_FN && cbody->as.fn.arg_full_types) {
            for (uint8_t ci = 0; ci < poly->as.forall_.n_constraints; ci++) {
                TypeClass *tc = poly->as.forall_.constraint_classes[ci];
                uint8_t    vi = poly->as.forall_.constraint_var_idx[ci];
                if (!tc || vi >= poly->as.forall_.n_vars) continue;
                const char *vname = poly->as.forall_.var_names[vi];
                if (!vname) continue;
                /* Pin the bound var to a concrete type: the first body parameter
                 * whose full type is a TY_TYVAR named `vname` binds it to the
                 * matching actual argument's type. */
                Type concrete;
                bool pinned = false;
                /* True when `f` is pinned NESTED inside a function-typed
                 * parameter (the van Laarhoven lens shape `g : (-> A (f A))`,
                 * MB4 branch below).  Only this pin gates the wide-by-value
                 * functor rejection (TUR-E0309); the direct-argument MB2 pin's
                 * aggregate `(f a)` crossing IS supported (MB2.5). */
                bool nested_functor_pin = false;
                for (uint32_t j = 0;
                     j < n_args && j < (uint32_t)cbody->as.fn.arity; j++) {
                    const Type *aft = cbody->as.fn.arg_full_types[j];
                    if (aft && aft->kind == TY_TYVAR && aft->as.tyvar_.name &&
                        strcmp(aft->as.tyvar_.name, vname) == 0) {
                        concrete = args[j]->type;
                        pinned = true;
                        break;
                    }
                    /* MB2 (constrained-hkt-forall-mode-b-plan): a *higher-kinded*
                     * constraint (`Functor f`) pins `f` to the container
                     * constructor of a `(f a)`-typed argument -- decompose the
                     * actual `(Box int)` to its base constructor `Box`.  The
                     * instance lookup then resolves `Functor Box`. */
                    if (aft && aft->kind == TY_APP && aft->as.app.fn &&
                        aft->as.app.fn->kind == TY_TYVAR &&
                        aft->as.app.fn->as.tyvar_.name &&
                        strcmp(aft->as.app.fn->as.tyvar_.name, vname) == 0 &&
                        args[j]->type.kind == TY_APP) {
                        const Type *base = &args[j]->type;
                        while (base->kind == TY_APP && base->as.app.fn)
                            base = base->as.app.fn;
                        concrete = *base;
                        pinned = true;
                        break;
                    }
                    /* MB4 (constrained-hkt-forall-mode-b-plan): the constraint
                     * var `f` may appear NESTED inside a body parameter rather
                     * than as a top-level `(f a)` argument -- a van Laarhoven lens
                     * takes `g : (-> A (f A))`, so `f` heads the RESULT of a
                     * function-typed parameter.  Structurally collect the bindings
                     * from this argument (call_collect_type_bindings recurses
                     * through the fn type, binding `f := (Const int)` from a
                     * concrete `g : (-> int (Const int int))`) and pin `f` to its
                     * bound constructor, decomposed to the base head so the
                     * instance lookup resolves `Functor Const`. */
                    if (aft) {
                        CallTypeBinding fbinds[16]; uint8_t fnb = 0;
                        if (call_collect_type_bindings(aft, args[j]->type,
                                                       fbinds, &fnb)) {
                            for (uint8_t bi = 0; bi < fnb; bi++) {
                                if (!fbinds[bi].name ||
                                    strcmp(fbinds[bi].name, vname) != 0)
                                    continue;
                                const Type *base = &fbinds[bi].type;
                                while (base->kind == TY_APP && base->as.app.fn)
                                    base = base->as.app.fn;
                                if (base->kind != TY_TYVAR &&
                                    base->kind != TY_UNKNOWN) {
                                    concrete = *base;
                                    pinned = true;
                                    nested_functor_pin = true;
                                }
                                break;
                            }
                        }
                        if (pinned) break;
                    }
                }
                /* turi-dict-passing-plan probe (2026-08-16): the constraint var
                 * may appear ONLY in the forall body's RESULT type --
                 * `make : (forall m. Applicative m => int -> (m int))` -- with
                 * no argument to pin from.  The old code deferred with a
                 * comment claiming the return context would resolve it, but
                 * nothing downstream did: the dict was silently NOT prepended
                 * while the callee wrapper still expects it as a leading arg,
                 * so `(g 7)` called a 3-param wrapper with 2 args and the int
                 * argument was dereferenced as the dict (SIGSEGV past a clean
                 * compile; turi, which resolves per-instance at run time, got
                 * it right).  Pin from the RT expected-type channel when the
                 * call site carries one (ascription / typed let), structurally
                 * matching the declared result exactly as MB4 matches an
                 * argument. */
                if (!pinned && poly_expected &&
                    cbody->as.fn.result_full_type) {
                    CallTypeBinding rbinds[16]; uint8_t rnb = 0;
                    if (call_collect_type_bindings(cbody->as.fn.result_full_type,
                                                   *poly_expected,
                                                   rbinds, &rnb)) {
                        for (uint8_t bi = 0; bi < rnb; bi++) {
                            if (!rbinds[bi].name ||
                                strcmp(rbinds[bi].name, vname) != 0)
                                continue;
                            const Type *base = &rbinds[bi].type;
                            while (base->kind == TY_APP && base->as.app.fn)
                                base = base->as.app.fn;
                            if (base->kind != TY_TYVAR &&
                                base->kind != TY_UNKNOWN) {
                                concrete = *base;
                                pinned = true;
                            }
                            break;
                        }
                    }
                }
                /* Still unpinned: the instantiation is ambiguous at this call
                 * site and the emitted call would be the silent ABI mismatch
                 * above -- reject instead. */
                if (!pinned) {
                    diag_emit(DIAG_ERROR, call->span,
                              "rank-2 constraint '%s %s' cannot be resolved at "
                              "this call site: no argument mentions '%s', and "
                              "no expected result type is in scope to pin it. "
                              "Ascribe the call result (e.g. (:: (f ...) (T "
                              "...))) or bind it with a typed let so the "
                              "instance can be chosen",
                              (tc->name && tc->name->name) ? tc->name->name
                                                           : "?",
                              vname, vname);
                    return NULL;
                }
                /* Only discharge against a ground type -- a tyvar/unknown/applied
                 * binding means the call sits inside another generic body and the
                 * obligation is still abstract (same guard as elab_call.c:4975). */
                if (concrete.kind == TY_TYVAR || concrete.kind == TY_UNKNOWN ||
                    concrete.kind == TY_APP) continue;
                /* van-laarhoven-functor-must-be-int-carrier: the mode-B carrier
                 * is a single int64 word, so a functor whose `(f a)` is a wide
                 * by-value aggregate -- a non-opaque, non-`:heap` single-variant
                 * flat product (a `:copy` struct / record ADT, more than one
                 * word of state) -- does not fit the one-word carrier directly.
                 * VBM4 handles it by boxing the aggregate into the carrier (Path
                 * A) rather than reinterpreting int64<->struct-pointer (which
                 * used to segfault at runtime).  An opaque (`is_opaque`, a named
                 * int64 -- `Const`/`Identity`) and a :heap def (a typed pointer)
                 * are BOTH one carrier word and skip boxing; the direct-argument
                 * MB2 shape supports aggregate `(f a)` (MB2.5), so only the
                 * nested-fn (lens) pin takes the box. */
                /* WF1 (van-laarhoven-wide-functor-carrier-plan): the wide-by-value
                 * functor is boxed across the lens crossings (Path A).  Graduated
                 * 2026-07-04 (VBM4) -- this is unconditional now; TUR-E0309 is
                 * retired.  The wide-ness test below (non-opaque, non-:heap
                 * flat-product) is the only gate, so carrier-compatible functors
                 * are untouched. */
                if (nested_functor_pin && concrete.kind == TY_ADT) {
                    const AdtDef *fd = concrete.as.adt_.def;
                    if (fd && !fd->is_opaque && !fd->is_heap &&
                        adt_is_flat_product(fd)) {
                        /* VBM1 (van-laarhoven-monomorphization-plan): this is a
                         * wide by-value functor pinned through the nested-fn lens
                         * shape -- exactly the class Path B specializes.  Record
                         * a spec key (enclosing fn, callee, functor, focus,
                         * whole) so `--dump-mono-specs` can surface the keying
                         * before VBM2 wires per-spec emit.  Registry-only: the
                         * Path A carrier-box path above still does the codegen.
                         * Graduated (vl-wide-mono, 2026-07-05): registration is
                         * unconditional; a composed lens is later gated back to
                         * Path A in mono_specs.c (lens_is_simple_for_pathb). */
                        {
                            const char *enclosing =
                                (e->current_fn_name && e->current_fn_name->name)
                                    ? e->current_fn_name->name : "?";
                            const char *callee =
                                (fn_binding && fn_binding->name &&
                                 fn_binding->name->name)
                                    ? fn_binding->name->name : "?";
                            /* focus A = domain of the functor-wrapping arg
                             * `g : (-> A (f A))`; whole S = the non-fn lens arg.
                             * Scan the forall-body params against the actuals. */
                            char focus_buf[96];
                            char whole_buf[96];
                            snprintf(focus_buf, sizeof focus_buf, "?");
                            snprintf(whole_buf, sizeof whole_buf, "?");
                            for (uint32_t wk = 0;
                                 wk < n_args &&
                                 wk < (uint32_t)cbody->as.fn.arity; wk++) {
                                const Type *waft = cbody->as.fn.arg_full_types
                                    ? cbody->as.fn.arg_full_types[wk] : NULL;
                                if (waft && waft->kind == TY_FN) {
                                    if (args[wk] &&
                                        args[wk]->type.kind == TY_FN &&
                                        args[wk]->type.as.fn.arity >= 1) {
                                        Type dom = (args[wk]->type.as.fn.arg_full_types &&
                                                    args[wk]->type.as.fn.arg_full_types[0])
                                            ? *args[wk]->type.as.fn.arg_full_types[0]
                                            : type_from_kind(
                                                  args[wk]->type.as.fn.arg_kinds[0]);
                                        snprintf(focus_buf, sizeof focus_buf,
                                                 "%s", type_name(dom));
                                    }
                                } else if (args[wk]) {
                                    snprintf(whole_buf, sizeof whole_buf,
                                             "%s", type_name(args[wk]->type));
                                }
                            }
                            mono_spec_register(enclosing, callee,
                                               type_name(concrete),
                                               focus_buf, whole_buf,
                                               vname, &concrete, fn_binding);
                        }
                    }
                }
                TypeClassInstance *inst = typeclass_env_lookup_instance(
                    &e->typeclass_env, tc, &concrete, 1);
                if (!inst) {
                    diag_emit(DIAG_ERROR, call->span,
                              "no '%s' instance for '%s' at this rank-2 "
                              "instantiation site -- required by the constraint "
                              "on '%s' in the poly fn's forall type (TUR-E0305)",
                              (tc->name && tc->name->name) ? tc->name->name : "?",
                              type_name(concrete), vname);
                    return NULL;
                }
                /* MB1 (constrained-hkt-forall-mode-b-plan): materialize the
                 * resolved instance's dictionary as a leading carrier argument so
                 * the callee (a dict-clone) dispatches its class method through it
                 * at runtime.  Bare-value EX_DICT form ->
                 * `(int64_t)(intptr_t)(&dict_C_T_singleton)`. */
                if (mb1_n_dicts < 16) {
                    Expr *de = expr_new(e->arena, EX_DICT, TYPE_PTR_VOID, call->span);
                    de->as.dict_.instance = inst;
                    de->as.dict_.method_name[0] = '\0';
                    mb1_dicts[mb1_n_dicts++] = de;
                }
            }
        }
    }

    /* Determine return type by instantiation.
     * For (forall [a] (-> a a)): result matches first arg's type.
     * For (forall [s] (-> s int)): result is int (concrete). */
    TypeKind result_kind = TY_INT;
    const Type *result_full = NULL;
    if (poly && poly->kind == TY_FORALL) {
        const Type *body = poly->as.forall_.body;
        if (body && body->kind == TY_FN) {
            const Type *rfull = body->as.fn.result_full_type;
            if (rfull && rfull->kind == TY_TYVAR) {
                /* Result is a type variable — instantiate from first arg's type.
                 * (The legacy anonymous TY_STRUCT{def=NULL} placeholder no longer
                 * exists; a bare-tyvar result is always the named TY_TYVAR from
                 * Direction A step 2a --
                 * see docs/archive/history/open-binder-skolems-not-distinguishable.md.) */
                result_kind = (n_args > 0 && args[0]) ? args[0]->type.kind : TY_INT;
                /* Slice 3 (constrained-hkt-forall codegen): when the result
                 * tyvar is instantiated to a by-value aggregate (e.g. `(Option
                 * int)`), carry its FULL type so the carrier unbox recognizes it
                 * -- otherwise type_from_kind(TY_APP) drops the def and the
                 * aggregate result is silently erased.  Pin the result tyvar to
                 * the argument whose body param names it; fall back to arg 0
                 * (the classic `a -> a` shape). */
                const Type *pin = NULL;
                if (rfull->as.tyvar_.name && body->as.fn.arg_full_types) {
                    for (uint32_t j = 0;
                         j < n_args && j < (uint32_t)body->as.fn.arity; j++) {
                        const Type *aft = body->as.fn.arg_full_types[j];
                        if (aft && aft->kind == TY_TYVAR && aft->as.tyvar_.name &&
                            strcmp(aft->as.tyvar_.name, rfull->as.tyvar_.name) == 0) {
                            pin = &args[j]->type;
                            break;
                        }
                    }
                }
                if (!pin && n_args > 0 && args[0]) pin = &args[0]->type;
                if (pin && (pin->kind == TY_APP || pin->kind == TY_ADT)) {
                    Type *rf = (Type *)arena_alloc(e->arena, sizeof(Type));
                    *rf = *pin;
                    result_full = rf;
                    result_kind = pin->kind;
                }
            } else if (rfull && rfull->kind == TY_APP) {
                /* Slice 3 (constrained-hkt-forall codegen): the result is `(f a)`
                 * (e.g. a lens/optic `(f a)` or `(f int) -> (f int)`).
                 * Instantiate the bound vars from the arguments via the
                 * structural unifier so the concrete container type (`(Option
                 * int)`) flows out instead of a def-less TY_APP. */
                result_kind = rfull->kind;
                if (body->as.fn.arg_full_types) {
                    CallTypeBinding binds[16];
                    uint8_t nb = 0;
                    for (uint32_t j = 0;
                         j < n_args && j < (uint32_t)body->as.fn.arity; j++) {
                        if (body->as.fn.arg_full_types[j])
                            call_collect_type_bindings(body->as.fn.arg_full_types[j],
                                                       args[j]->type, binds, &nb);
                    }
                    /* turi-dict-passing-plan probe: a bound var that no
                     * argument mentions (`int -> (m int)`) instantiates from
                     * the expected result type -- the same source the MB1
                     * dict pin used above.  Args bind first, so an
                     * arg-derived binding still wins. */
                    if (poly_expected)
                        call_collect_type_bindings(rfull, *poly_expected,
                                                   binds, &nb);
                    Type inst = call_instantiate_type(e, rfull, binds, nb);
                    if (inst.kind == TY_APP || inst.kind == TY_ADT) {
                        Type *rf = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *rf = inst;
                        result_full = rf;
                        result_kind = inst.kind;
                    }
                }
            } else if (rfull && rfull->kind == TY_FN) {
                /* hrt-curried-result GRADUATED 2026-07-06: the forall body's
                 * result is itself a function -- `forall a. a -> (a -> a)`, i.e.
                 * `(l x)` yields a CLOSURE that `((l x) y)` then applies (van
                 * Laarhoven optic composition).  Instantiate the result fn type
                 * through the argument bindings so the outer call carries a
                 * concrete, callable `TY_FN` type instead of the bare
                 * `type_from_kind(TY_FN)` (which is non-callable -- TUR-E0002
                 * "returns ?").  Mirrors the TY_APP result branch below. */
                result_kind = TY_FN;
                if (body->as.fn.arg_full_types) {
                    CallTypeBinding binds[16];
                    uint8_t nb = 0;
                    for (uint32_t j = 0;
                         j < n_args && j < (uint32_t)body->as.fn.arity; j++) {
                        if (body->as.fn.arg_full_types[j])
                            call_collect_type_bindings(body->as.fn.arg_full_types[j],
                                                       args[j]->type, binds, &nb);
                    }
                    Type inst = call_instantiate_type(e, rfull, binds, nb);
                    if (inst.kind == TY_FN) {
                        Type *rf = (Type *)arena_alloc(e->arena, sizeof(Type));
                        *rf = inst;
                        result_full = rf;
                    }
                }
            } else if (rfull) {
                result_kind = rfull->kind;
            } else {
                result_kind = body->as.fn.result_kind;
            }
        }
    } else if (poly && poly->kind == TY_FN) {
        /* F5 typed `:fn` carrier: the concrete signature fixes the result kind,
         * so a float/cstr/ptr result round-trips (the carrier stores a natively
         * typed thunk; emit casts fn.fn to the concrete R(*)(void*, A...)). */
        result_kind = poly->as.fn.result_kind;
        result_full = poly->as.fn.result_full_type;
    }

    /* MB4 (constrained-hkt-forall-mode-b-plan): box thin function-typed args to a
     * uniform fat box.  Done HERE -- after constraint pinning and result-type
     * determination, which both read the argument's real function type -- because
     * EX_FN_TO_FAT rewrites the arg's static type to `ptr<void>`, which would
     * otherwise hide `f` from the `(-> A (f A))` pinning.  A function value
     * crossing the poly carrier is a uniform fat box (the dict-clone fat-dispatches
     * its function params through slot 0, make_dict_clone), so a thin
     * (non-capturing) fn argument must be boxed; a capturing closure already
     * carries one (TY_PTR_VOID / boxed TY_FN) and is left untouched.
     *
     * Only the CONSTRAINED (dict-pass) path installs a dict-clone that
     * fat-dispatches fn params through slot 0, so restrict the boxing to a
     * constrained forall.  An UNconstrained curried rank-2 forall (plain HRT,
     * e.g. `(forall [a] (-> (-> a a) (-> a a)))`) has no dict-clone; its callee
     * takes a thin fn pointer, and boxing it would fat-dispatch a thin pointer
     * -> SIGSEGV (hrt-curried-fn-result).  Before forall-dict-pass graduated
     * this whole block was gated behind the flag, so a flagless unconstrained
     * forall never reached it. */
    if (poly && poly->kind == TY_FORALL &&
        poly->as.forall_.n_constraints > 0) {
        const Type *pbody = poly->as.forall_.body;
        if (pbody && pbody->kind == TY_FN && pbody->as.fn.arg_full_types) {
            for (uint32_t i = 0; i < n_args &&
                 i < (uint32_t)pbody->as.fn.arity; i++) {
                const Type *aft = pbody->as.fn.arg_full_types[i];
                if (aft && aft->kind == TY_FN &&
                    args[i]->type.kind == TY_FN &&
                    !args[i]->type.as.fn.boxed &&
                    args[i]->type.as.fn.arity >= 1 &&
                    args[i]->type.as.fn.arity <= 5) {
                    Expr *shim = expr_new(e->arena, EX_FN_TO_FAT,
                                          TYPE_PTR_VOID, args[i]->span);
                    shim->as.fn_to_fat_.inner = args[i];
                    args[i] = shim;
                }
            }
        }
    }

    /* WF1/WF2 (van-laarhoven-wide-functor-carrier-plan): when a functor-wrapping
     * fn argument `g : (-> A (f A))` is passed to a van Laarhoven lens whose `f`
     * is a WIDE by-value aggregate functor, its `(f A)` result would be returned
     * by value -- but `g` crosses the mode-B poly carrier and is fat-dispatched
     * (slot 0) by the generic dict-clone as an int64-returning thunk.  Flag the
     * closure/fn FnDef so emit gives it the int64 carrier return type and
     * heap-boxes the aggregate (the box the lens's poly-carrier boundary already
     * unboxes).  Unconditional since VBM4 graduated vl-wide-functor; the
     * wide-ness test below still skips carrier-compatible functors (opaque
     * `Const`/`Identity`, `:heap`), which are one word. */
    if (poly && poly->kind == TY_FORALL) {
        const Type *pbody = poly->as.forall_.body;
        if (pbody && pbody->kind == TY_FN && pbody->as.fn.arg_full_types) {
            for (uint32_t i = 0; i < n_args &&
                 i < (uint32_t)pbody->as.fn.arity; i++) {
                const Type *aft = pbody->as.fn.arg_full_types[i];
                if (!aft || aft->kind != TY_FN) continue;
                /* Reach the closure/fn FnDef behind the (possibly boxed /
                 * ascribed) arg -- an inline `(fn ...)` may arrive wrapped in an
                 * EX_FN_TO_FAT shim and/or an erased EX_ASCRIBE. */
                Expr *a = args[i];
                while (a && (a->kind == EX_FN_TO_FAT || a->kind == EX_ASCRIBE))
                    a = (a->kind == EX_FN_TO_FAT) ? a->as.fn_to_fat_.inner
                                                  : a->as.ascribe_.inner;
                FnDef *gfd = NULL;
                if (a && a->kind == EX_CLOSURE && a->as.closure_.closure)
                    gfd = a->as.closure_.closure->fn;
                else if (a && a->kind == EX_VAR && a->as.var.binding)
                    gfd = a->as.var.binding->source_fn_def;
                if (!gfd || !gfd->binding || gfd->binding->type.kind != TY_FN)
                    continue;
                const Type *grf = gfd->binding->type.as.fn.result_full_type;
                if (!grf) continue;
                /* The `(f A)` result must be a wide by-value aggregate functor:
                 * a non-opaque, non-:heap flat-product ADT (matches the E0309
                 * gate's functor test).  Key on the FUNCTOR def's layout, NOT on
                 * the focus `A` being concrete -- in the generic `set`/`over`
                 * shape the closure result is `(Identity A)` with `A` still a
                 * type variable, but its box-ness is decided by `Identity`, not
                 * by `A` (plan Open Question #3).  A carrier-compatible functor
                 * (opaque `Const`/`Identity`, `:heap`) is one word and skips. */
                const AdtDef *ad = (grf->kind == TY_APP) ? type_adt_app_def(grf)
                    : (grf->kind == TY_ADT ? grf->as.adt_.def : NULL);
                bool wide = ad && !ad->is_opaque && !ad->is_heap &&
                            adt_is_flat_product(ad);
                if (wide) {
                    gfd->box_aggregate_result = true;
                }
            }
        }
    }

    Type result_ty = result_full ? *result_full : type_from_kind(result_kind);
    Expr *out = expr_new(e->arena, EX_CALL, result_ty, call->span);
    out->as.call_.fn_binding = fn_binding;
    out->as.call_.args = n_args > 0 ? args : NULL;
    out->as.call_.n_args = n_args;
    out->as.call_.fn_expr = NULL;
    out->as.call_.dict_arg = NULL;
    out->as.call_.is_poly_call = true;
    out->as.call_.poly_arg_mask = poly_arg_mask;
    /* MB1: prepend the resolved dictionaries as leading carrier args (constraint
     * order), matching the dict slots the wrapper carries.  Done after the
     * result-type / poly_arg_mask logic (which reasons about the real args) so
     * those are unperturbed; the emitted N-ary carrier call passes them through. */
    if (mb1_n_dicts > 0) {
        uint32_t total = (uint32_t)mb1_n_dicts + out->as.call_.n_args;
        Expr **na = (Expr **)arena_alloc(e->arena, total * sizeof(Expr *));
        for (uint8_t k = 0; k < mb1_n_dicts; k++) na[k] = mb1_dicts[k];
        for (uint32_t k = 0; k < out->as.call_.n_args; k++)
            na[mb1_n_dicts + k] = out->as.call_.args[k];
        /* Shift the nested-poly-arg mask by the number of prepended dicts. */
        out->as.call_.poly_arg_mask = poly_arg_mask << mb1_n_dicts;
        out->as.call_.args = na;
        out->as.call_.n_args = total;
    }
    return out;
}
