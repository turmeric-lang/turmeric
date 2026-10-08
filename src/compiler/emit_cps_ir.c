#include "emit_cps_ir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cps_ir.h"
#include "cps.h"
#include "builtins.h"
#include "globals.h"
#include "arena.h"
#include "expr.h"
#include "effect.h"   /* E2 taint: credit indirect fn-value effect rows */

/* The elaborator's complete free-variable walker (backs the closure-capture
 * pass).  Reused here to find every enclosing var a delegated *composite*
 * (match / while / for / do / let / set! / ...) references, so such a form can be
 * lifted into a continuation env.  Declared in elab_internal.h. */
Binding **collect_free_vars(const Expr *e, Binding **params, uint8_t n_params,
                            Binding **self_exclude, uint32_t n_self_exclude,
                            uint32_t *n_out);

/* B7 fwd decls (defined after the CE struct). */
static bool is_byref_mut(const Binding *b);
static void ctg_reset(void);   /* cps-self-tail-call-relies-on-sibling-call */
static const struct Binding *byref_set_target(const Expr *e);
static const struct Binding *set_mut_target(const Expr *e);
static const char *byref_cell_ctype(EmitCtx *ctx, const Binding *b);
static const char *byref_cell_ptr_ctype(EmitCtx *ctx, const Binding *b);

/* B4 (CT_MATCH emit): the ADT-aggregate field-access helpers.  Declared in
 * emit_internal.h, which this file does not include; forward-declared here so
 * emit_match can spell `(tur_adt_<Name> *)->as.<Ctor>._N` field reads exactly as
 * the direct emitter (emit_expr.c) does. */
char *mangle_field_name(const char *name);
char *mangle_adt_name(const char *name);
char *adt_field_member_path(const AdtDef *def, const CtorDef *ctor, uint32_t fi);
/* S1/findings 16: ground-truth return-type lookup for cps->direct call temps. */
const char *emit_sig_lookup_ret_ctype(const char *cname);

/* True for a NULL or `{}` (ERK_EMPTY) effect row.  Used to distinguish a
 * provably effect-free `TY_FN` param (safe to delegate an indirect call
 * through) from an effectful callback (whose call would need DK threading).
 * Defined in src/passes/effect.c. */
bool effect_row_is_empty(struct EffectRow *row);

/* =========================================================================
 * CPS-IR-to-C backend -- Phase C1.  See emit_cps_ir.h for the ABI and scope.
 *
 * Two responsibilities:
 *   1. Whole-program classification (ensure_S): which colored functions the
 *      backend emits.  A function is emittable iff its CTerm is in the C1 core
 *      subset (scalar types, no reset/shift/effects, no unsupported form) AND
 *      no join point in it must be reified onto the heap chain -- i.e. no
 *      non-tail call to another emittable function.  That last clause is a
 *      monotone fixpoint (removing a function from the set only turns its
 *      callers' cps->cps edges into ordinary synchronous calls, which never
 *      forces a heap join), so it converges.
 *   2. Emission (emit_cps_ir_try_fn): walk the CTerm and write the DK-ABI body
 *      plus a direct-entry wrapper.
 *
 * Join points (CT_LETCONT) lower to a C local + a forward label/goto (plan:
 * "local labels/gotos"): delivering a value to join j assigns the join
 * parameter and `goto L<j>`.  This is sufficient for every join whose
 * continuation stays on the C stack (if-merges and the result of a synchronous
 * call).  A join that would have to survive a cps->cps call needs a real
 * dk_frame; that is deliberately out of the C1 subset -- such functions fall
 * back -- and lands in a later phase.
 * ========================================================================= */

/* ---- the emittable value set (non-scalar Tier A) --------------------- *
 * A type is "slot-representable at Tier A" when its value fits the one-word DK
 * slot by a plain cast: any <=64-bit integer/bool, or a cstr/pointer.  See
 * docs/archive/cps-backend-non-scalar-values-plan.md.  Tier B (float, bit-
 * reinterpret) and Tier C (wide by-value aggregates, boxed) are not yet here.
 *
 * Deliberately absent: the OWNING pointers TY_REF / TY_RC / TY_WEAK / TY_LREF.
 * Do NOT add them here -- they are never bare slot values.  A bare owning
 * pointer cannot even be a function result / continuation payload in the source
 * language (`(defn f [] : rc<int> ...)` is a type error); owning values cross a
 * continuation only as FIELDS of a struct / ADT, so their slot discipline is the
 * enclosing aggregate's drop glue (N3), not a per-pointer cast.  The non-owning
 * borrows TY_REF_IMMUT / TY_REF_MUT are also omitted: a borrow threaded through a
 * continuation would outlive its referent, which the borrow checker rejects.
 * See docs/archive/cps-backend-owning-pointers-plan.md. */
static bool slot_ty(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_INT64: case TY_BOOL:
        case TY_INT8: case TY_INT16: case TY_INT32:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_CSTR:
        case TY_PTR_VOID:   /* raw pointer: Copy, non-owning -- casts through the slot */
        case TY_HANDLER:    /* B3: a first-class handler value is a one-word
                             * tur_handler_table_t* -- crosses the slot like a ptr,
                             * so a handler-typed param/return is CPS-admissible
                             * (e.g. run-with taking a handler param). */
        case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32:  /* Tier B: bit-reinterpret */
            return true;
        default:
            return false;
    }
}

static const char *binder_ctype(EmitCtx *ctx, TypeKind ty);
static const char *binder_ctype_full(EmitCtx *ctx, TypeKind ty, const Type *t);

/* G3a: when set (while a colored-generic MONOMORPH is being CPS-emitted as a
 * self-contained island, and during the --dump-cps-mono probe), the slot-tier and
 * signature gates resolve a type through the active monomorph specialization
 * before reading it -- so the generic body's tyvar types (`A`, `(Option A)`) are
 * classified / boxed / bit-reinterpreted at their CONCRETE monomorph types.  NULL
 * in every ordinary emit path, so all of this is a strict no-op there. */
static EmitCtx *g_cps_mono_resolver = NULL;

/* Resolve `*t` through the active monomorph spec when the hook is on; else return
 * `*t` unchanged.  `out` provides storage for the resolved Type.  Returns the
 * type to use (either `out` or `t`). */
static const Type *cps_resolve_ty(const Type *t, Type *out) {
    if (g_cps_mono_resolver && t) { *out = emit_resolve_type(g_cps_mono_resolver, *(Type *)t); return out; }
    return t;
}

/* Forward decls used by ensure_S's G3b mono-template classification (defined
 * below). */
static bool mono_sig_ok(const FnDef *fd, const EmitAbiSpecialization *spec);
static bool is_cps_island(const CTerm *t, const Symbol **handled, int nh);

/* The AdtDef at the head of a struct / ADT / ADT-app type, or NULL. */
static const AdtDef *slot_agg_def(const Type *t) {
    if (!t) return NULL;
    if (t->kind == TY_ADT) return t->as.adt_.def;
    if (t->kind == TY_APP) return type_adt_app_def((Type *)t);
    return NULL;
}

/* Tier C: a by-value aggregate that must be BOXED (heap-copied) to cross the
 * one-word DK slot -- as opposed to a carrier-ABI aggregate whose bit pattern
 * already IS the int64 carrier (would ride the slot by a plain cast) or a scalar
 * (Tier A/B).
 *
 * Restricted to owning-free flat products (`needs_drop_glue == false`): the box
 * is then a pure bitwise value copy with no refcount / drop concern, so it is
 * safe to heap-copy on store and deref on load even under a multi-shot
 * continuation (each crossing gets its own independent copy).  Aggregates with
 * owning fields (rc / ref / weak) still fall back -- their crossing needs
 * retain-on-copy plus drop glue (N3-O2).  Carrier heap-ADT / parametric-app
 * handles also stay out (bit-copying an owning int64 handle would duplicate its
 * refcount).  The def must be present: a bare surface annotation (e.g. a NULL
 * `return_type` def) is not enough -- callers pass the body/value Type, which
 * carries the real monomorphized def. */
/* SR2b: a CONCRETE parametric monomorph that rides the int64 carrier -- one
 * the SR2a by-value predicate declines (self-recursive, :heap, GADT, a
 * fixpoint partner's functor app), or any of them under the
 * TUR_SR2_APP_SUM_BYVALUE=0 bisection hatch.  Its C
 * spelling IS int64_t (type_c_name's TY_APP arm answers the carrier), so it
 * crosses a DK slot exactly like a scalar: plain cast, no box, no drop
 * concern (the carrier ctor's box is process-lifetime today).  Before
 * Option/Result were sums this shape never reached the gate -- their record
 * monomorphs were by-value products and took slot_box_ty. */
static bool slot_carrier_app(const Type *t) {
    if (!t) return false;
    Type _r; t = cps_resolve_ty(t, &_r);
    if (t->kind != TY_APP) return false;
    if (!type_app_is_concrete_adt((Type *)t)) return false;
    const char *cn = type_c_name(*(Type *)t);
    return cn && strcmp(cn, "int64_t") == 0;
}

/* colored-generic-tyvar-elemented-param-sig-rejects: the ERASED counterpart of
 * slot_carrier_app, for a monomorph SPEC's materialized signature only.  When
 * nothing pins a colored generic's tyvar -- `(peek (ok 1))` against `peek [E]
 * [r : (Result int E)]`, where `(ok 1)` is `(Result int ?)` -- the ABI scan
 * still mints a clone (`peek__spec__int64_t_int64_t`), with the arg
 * materialized as the erased app, whose C spelling is the int64 carrier.  That
 * clone is the only candidate that can carry the `perform` (the base must keep
 * sig-rejecting so it stays a mono-template), and its arg crosses a DK slot as
 * the plain word it already is, exactly like the concrete SR2b case.  The
 * direct emitter's forward decl and emit_params both spell it `int64_t`, so
 * the `__cps` entry and the wrapper agree with every caller.  Not admitted at
 * the BASE gate (sig_slot_ok): the base name is shared by every clone, and
 * admitting it there is what double-emitted the map-eq drivers' joins. */
static bool erased_adt_carrier(const Type *t);
static bool slot_erased_carrier_app(const Type *t) {
    if (!t) return false;
    Type _r; t = cps_resolve_ty(t, &_r);
    if (t->kind != TY_APP) return false;
    if (!erased_adt_carrier(t)) return false;
    const char *cn = type_c_name(*(Type *)t);
    return cn && strcmp(cn, "int64_t") == 0;
}

static bool slot_box_ty(const Type *t) {
    if (!t) return false;
    Type _r; t = cps_resolve_ty(t, &_r);
    /* perform-in-fn-with-any-param-has-no-cps-lowering: `any` is a Tier C
     * aggregate in every way that matters here -- a fixed two-word
     * `tur_tagged_t` the direct emitter passes and returns BY VALUE
     * (type_struct_pass_by_ptr says so), owning-free as far as the compiler
     * tracks (the payload box behind `val` has no drop glue anywhere; that is
     * docs/reported/any-struct-box-leak-per-widen.md, not an owning field).
     * It simply is not an AdtDef, so the by-value-product test below cannot
     * see it and the whole function fell out of the CPS subset: `fn_sig_ok`'s
     * param gate rejected it, and a `perform` anywhere in such a function then
     * reached the direct emitter, which has no lowering for one.
     *
     * Answering here rather than at the param gate is deliberate: this is the
     * one predicate param admission, slot_store/slot_load, and cap_ty_ok all
     * consult, so an `any` that merely crosses a signature and one that has to
     * live across a `perform` get consistent treatment -- boxed into the
     * one-word slot on the way in, unboxed (and freed, on the consuming load)
     * on the way out.
     *
     * Note this is a narrower question than type_has_concrete_codegen_layout,
     * which deliberately rejects TY_ANY (types.c): that gate is about by-value
     * MONOMORPH FIELDS, where a 16-byte member would be an ABI change. Crossing
     * a DK slot is a local box/unbox and needs no such thing. */
    if (t->kind == TY_ANY) return true;
    /* A heap-passed ADT / struct (`(Vec int)` -> `tur_adt_Vec__int *`, ...) is
     * already a pointer (the int64 carrier), NOT a by-value product to heap-copy.
     * Some heap ADT defs still have product *shape* (`{ len; cap; data }`), so
     * type_is_byvalue_adt_product can report true for them -- but boxing one would
     * double-indirect the handle (store a T** where a T* is expected) and later
     * free a non-box.  Exclude them here so they are neither admitted through the
     * Tier C by-value gate nor routed through slot_store/slot_load's box/unbox. */
    if (type_is_heap_adt(*(Type *)t) || type_is_heap_struct(*(Type *)t)) return false;
    if (!type_is_byvalue_adt_product(*(Type *)t)) return false;
    const AdtDef *d = slot_agg_def(t);
    return d && !d->needs_drop_glue;
}

/* A heap-passed ADT / struct handle (`(Vec int)` -> `tur_adt_Vec__int *`, a heap
 * ADT node, ...).  Its C representation IS a pointer -- the int64 carrier -- so
 * it crosses the DK slot by a plain cast (like TY_PTR_VOID), never a box.  It is
 * an *owning* handle, so it is admitted through the slot / signature gate but NOT
 * through the capture gate (cap_ty_ok): a handle live across a control op is
 * captured into a continuation env, where sharing the owning pointer is only
 * sound in a single-shot continuation -- so cap_add gates a capture of it on
 * g_cap_single_shot (a handler CASE body, which runs per-perform, still bails).
 * As a slot value / atom / call-arg it just threads the pointer by move, matching
 * the direct emitter (whose owning ops are delegated via CT_LETRAW either way). */
/* B8: a session-protocol type is an opaque `void*` channel handle (a
 * `TurChannel *` / session-pair box), a pointer carrier that crosses the DK slot
 * by a plain cast exactly like a heap-ADT handle. */
static bool type_is_session(TypeKind k) {
    return k == TY_SESSION || k == TY_SESSION_REC || k == TY_SESSION_PAIR
        || k == TY_SESSION_RECV_PAIR || k == TY_SESSION_OFFER
        || k == TY_ROLE;   /* B8: a multi-party Role endpoint is a void* carrier too */
}

static bool carrier_handle_ok(const Type *t) {
    if (!t) return false;
    if (type_is_session(t->kind)) return true;
    return (type_is_heap_adt(*(Type *)t) || type_is_heap_struct(*(Type *)t));
}

/* A value can cross the DK slot at the current tier: a Tier A/B scalar, a Tier C
 * owning-free by-value aggregate (boxed), or a heap-ADT/struct handle (plain-cast
 * int64 carrier).  This is the whole-Type gate used by classification wherever
 * the full Type is available (the bare-TypeKind slot_ty stays for scalar-only
 * sites).  It is deliberately WIDER than cap_ty_ok: a value may cross a slot by a
 * single move even when it may not be duplicated into a capture env. */
/* An ADT/struct application with an UNRESOLVED type-variable argument -- the
 * erased generic case, e.g. `(none) : (Option A)` where `A` never gets pinned
 * (both args are the nullary `none`, so nothing constrains the element).  In a
 * polymorphic/erased context such a value rides the uniform int64 CARRIER ABI:
 * the direct emitter passes it as a plain `int64_t` (see the generic
 * `option_hyeq_qu(int64_t, int64_t, int64_t)` carrier signature the erased call
 * resolves to), so it crosses a DK slot / a cps->direct call arg as one word,
 * exactly like `carrier_handle_ok`.  It cannot key a concrete monomorph clone
 * (there is no element type), so a mono-template call with such an arg takes the
 * emit's no-clone cps->direct fallback (CT_TAILCALL: `mclone == NULL` -> direct
 * call to the generic carrier callee).  Admitting it here lets the ERASED-generic
 * caller (`test-option-eq-nones`) CPS-emit instead of evicting. */
static bool erased_adt_carrier(const Type *t) {
    if (!t) return false;
    Type tt = *(Type *)t;
    if (tt.kind != TY_APP) return false;
    AdtDef *def = NULL; Type args[16]; uint8_t n_args = 0;
    if (!type_extract_adt_app(&tt, &def, args, &n_args)) return false;
    for (uint8_t i = 0; i < n_args; i++)
        if (args[i].kind == TY_TYVAR || args[i].kind == TY_UNKNOWN) return true;
    return false;
}

/* True when `t` mentions an UNRESOLVED type variable anywhere -- a bare tyvar, or
 * an ADT application with a tyvar/unknown argument (recursively).  A carrier param
 * / return of such a type belongs to a GENERIC TEMPLATE: its `type_c_name` erases
 * to the uniform `int64_t` carrier, so admitting it into fn_sig_ok would emit a
 * colored generic `<fn>__cps(int64_t, ...)` whose concrete monomorphic callers
 * pass real struct args (`tur_adt_Option__int`) -- a `conflicting types` C error.
 * The template must sig-REJECT (stay a mono_template, per the g_ents.mono_template
 * design) so callers thread the concrete monomorph clone (`<fn>__spec__..._cps`)
 * or, for a fully-erased call, cps->direct to the generic int64 carrier.  This is
 * strictly a TEMPLATE-signature gate; the erased VALUE-crossing path
 * (erased_adt_carrier via slot_ok_t) is unaffected. */
static bool type_has_unresolved_tyvar(const Type *t) {
    if (!t) return false;
    if (t->kind == TY_TYVAR || t->kind == TY_UNKNOWN) return true;
    if (t->kind == TY_APP) {
        Type tt = *(Type *)t;
        AdtDef *def = NULL; Type args[16]; uint8_t n_args = 0;
        if (type_extract_adt_app(&tt, &def, args, &n_args)) {
            for (uint8_t i = 0; i < n_args; i++)
                if (type_has_unresolved_tyvar(&args[i])) return true;
        }
    }
    return false;
}

/* B4 (cps-tramp-resume): a BOXED TAGGED-SUM ADT (a `defdata` with >=2 ctors and
 * a field-bearing ctor, e.g. `Box`) is malloc'd and carried as an int64 POINTER
 * word -- its `type_c_name` is "int64_t" -- even though `def->is_heap` is false
 * (is_heap tracks the growable-container ADTs, not every boxed sum).  Like a
 * heap-ADT handle (`carrier_handle_ok`) it crosses a DK slot by a plain cast, so
 * admit it as a slot carrier.  A BY-VALUE product ADT keeps its aggregate
 * `type_c_name` (`tur_adt_<Name>`), so this correctly rejects it -- those ride
 * the Tier-C box path (`slot_box_ty`) instead.  Gated: flag-off keeps the
 * historical (is_heap-only) admission byte-identical. */
static bool adt_int64_carrier(const Type *t) {
    if (!t) return false;
    TypeKind k = t->kind;
    if (k != TY_ADT && k != TY_APP) return false;
    return strcmp(type_c_name(*(Type *)t), "int64_t") == 0;
}

static bool slot_ok_t(const Type *t, TypeKind k) {
    Type rt; const Type *r = cps_resolve_ty(t, &rt);
    if (r != t) k = r->kind;
    return slot_ty(k) || slot_box_ty(r) || carrier_handle_ok(r)
        || erased_adt_carrier(r) || adt_int64_carrier(r);
}

/* Signature-position slot gate (a colored function's PARAMS and RETURN).  It is
 * stricter than slot_ok_t: only a SCALAR (Tier A/B) type is admitted.  A
 * non-scalar signature -- a heap-ADT/struct HANDLE (`(Set cstr)` ->
 * `tur_adt_Set__cstr *`) OR a by-value ADT aggregate (`slot_box_ty`, e.g. a
 * `tur_adt_H` record) -- is rejected.  The CPS backend emits a colored function
 * as `f__cps(<concrete params>, DK*)` plus a direct-entry wrapper
 * `f(<concrete params>)`; when the same base name is ALSO reached through the
 * uniform carrier/dict ABI (a typeclass method dict is `_Bool(int64_t,int64_t)`,
 * and the base name is specialized both concretely and as an int64 carrier), the
 * CPS wrapper + `__cps` re-emit the concrete signature and collide with the
 * carrier specialization -- a `conflicting types` / duplicate-definition C error.
 * A scalar signature has a single int64/bool/double ABI that never diverges
 * between the carrier and concrete paths, so it is safe; a non-scalar-signature
 * colored function evicts to the direct emitter, which owns the carrier bridge.
 * (Non-scalars still cross DK slots fine as INTERNAL values via slot_ok_t -- this
 * gate is signature-only.) */
static bool sig_slot_ok(const Type *t, TypeKind k) {
    Type rt; const Type *r = cps_resolve_ty(t, &rt);
    if (r != t) k = r->kind;
    /* SR2b: a CONCRETE application whose C spelling IS the int64 carrier -- a
     * sum monomorph the SR2a by-value predicate declines (self-recursive,
     * :heap, GADT, fixpoint partner), or any of them under
     * TUR_SR2_APP_SUM_BYVALUE=0 -- crosses a DK slot as the plain word it
     * already is.
     * CONCRETE only, deliberately: a tyvar-elemented app (`(Option A)`,
     * `(Map A B)`) must keep rejecting, because the whole mono-template /
     * island machinery is built on the generic BASE sig-rejecting (see the
     * "sig-rejects itself so in_s stays false" invariants); admitting it
     * flipped map-eq drivers to candidates and double-emitted their CPS
     * joins.  Before Option/Result were sums this case never arose: a
     * concrete `(Option int)` was a by-value product and took slot_box_ty. */
    if (r->kind == TY_APP && type_app_is_concrete_adt((Type *)r) &&
        strcmp(type_c_name(*r), "int64_t") == 0)
        return true;
    return slot_ty(k);
}

/* Store a value into the one-word slot.  Returns a malloc'd C expression.
 *   Tier A: plain cast.
 *   Tier B: bit-reinterpret ((intptr_t)3.5 would truncate the fraction).
 *   Tier C: heap-copy the aggregate and store the pointer (unboxed on load). */
static char *slot_store(EmitCtx *ctx, TypeKind k, const Type *t, const char *e) {
    Buf b; buf_init(&b);
    Type _r; const Type *rt = cps_resolve_ty(t, &_r); if (rt != t) k = rt->kind; t = rt;
    if (slot_box_ty(t)) {
        const char *cty = emit_type_c_name(ctx, *(Type *)t);
        buf_printf(&b,
            "(intptr_t)({ %s *__bx = (%s *)malloc(sizeof(%s)); *__bx = (%s); __bx; })",
            cty, cty, cty, e);
    } else if (k == TY_FLOAT || k == TY_FLOAT64)
        buf_printf(&b, "(intptr_t)((union { double d; int64_t i; }){ .d = (%s) }).i", e);
    else if (k == TY_FLOAT32)
        buf_printf(&b, "(intptr_t)(uint32_t)((union { float f; uint32_t u; }){ .f = (%s) }).u", e);
    else
        buf_printf(&b, "(intptr_t)(%s)", e);
    buf_putc(&b, '\0');
    char *s = strdup(b.data); buf_free(&b); return s;
}

/* Load a value out of the one-word slot.  Returns a malloc'd C expression.
 *   Tier C: deref the boxed pointer to a value copy.  `consume` additionally
 *   frees the box -- only sound at a single-shot boundary (the root entry
 *   unwrap).  The continuation-internal boundaries pass consume=false and leak
 *   the box (matching the intentional DK-node leak): a multi-shot continuation
 *   could load the same slot more than once, so freeing there would double-free. */
static char *slot_load(EmitCtx *ctx, TypeKind k, const Type *t, const char *s, bool consume) {
    Buf b; buf_init(&b);
    Type _r; const Type *rt = cps_resolve_ty(t, &_r); if (rt != t) k = rt->kind; t = rt;
    if (slot_box_ty(t)) {
        const char *cty = emit_type_c_name(ctx, *(Type *)t);
        if (consume)
            buf_printf(&b,
                "({ %s *__bx = (%s *)(%s); %s __v = *__bx; free(__bx); __v; })",
                cty, cty, s, cty);
        else
            buf_printf(&b, "(*(%s *)(%s))", cty, s);
    } else if (k == TY_FLOAT || k == TY_FLOAT64)
        buf_printf(&b, "((union { double d; int64_t i; }){ .i = (int64_t)(%s) }).d", s);
    else if (k == TY_FLOAT32)
        buf_printf(&b, "((union { float f; uint32_t u; }){ .u = (uint32_t)(%s) }).f", s);
    else
        /* Plain cast to the value's real C type -- binder_ctype_full so a heap-ADT
         * handle (TY_ADT/APP/STRUCT) casts back to its pointer type, not int64. */
        buf_printf(&b, "(%s)(%s)", binder_ctype_full(ctx, k, t), s);
    buf_putc(&b, '\0');
    char *r = strdup(b.data); buf_free(&b); return r;
}

/* Like slot_store, but when the value is boxed (a Tier-C aggregate riding the
 * intptr slot as a malloc'd pointer) register the box in the per-run reap list
 * so it is freed at the outermost entry boundary.  Used at the sites whose box
 * is read with slot_load(consume=false) -- an effect arg carried through
 * dk_perform, shared read-only across a multi-shot resume -- and so is never
 * consume-freed at a load site (that would double-free the reaped box).  A
 * scalar slot is returned unchanged (no pointer to free).  See
 * docs/archive/cps-effect-perform-carrier-leak.md. */
static char *slot_store_reap(EmitCtx *ctx, TypeKind k, const Type *t, const char *e) {
    char *s = slot_store(ctx, k, t, e);
    Type _r; const Type *rt = cps_resolve_ty(t, &_r);
    if (slot_box_ty(rt)) {
        Buf b; buf_init(&b);
        buf_printf(&b, "__dk_reap_ptr(%s)", s);
        buf_putc(&b, '\0');
        free(s); s = strdup(b.data); buf_free(&b);
    }
    return s;
}

/* The Type a colored function's value has when delivered to KK_RET.  fd->return_type
 * can carry a NULL ADT def (the surface annotation is not def-resolved); the body's
 * type carries the real monomorphized def and the body's value is exactly what is
 * delivered, so prefer it for an aggregate return. */
static const Type *fn_ret_type(const FnDef *fd) {
    if (fd->body) {
        TypeKind bk = fd->body->type.kind;
        if ((bk == TY_ADT || bk == TY_APP || bk == TY_STRUCT)
            && slot_agg_def(&fd->body->type))
            return &fd->body->type;
        /* B6: a typeclass INSTANCE METHOD's declared return annotation is left
         * TY_UNKNOWN by elab_typeclasses (a placeholder never resolved), so an
         * effectful `:nil`-returning method would fail the sig gate on an
         * unresolved return.  Defer to the inferred body type (here TY_NIL), which
         * is what the direct emitter already uses to spell the `void` entry. */
        if (fd->binding && fd->binding->is_instance_method
            && fd->return_type.kind == TY_UNKNOWN && bk != TY_UNKNOWN)
            return &fd->body->type;
    }
    return &fd->return_type;
}

/* The C type used to declare a let-binder of type kind `ty` (TY_NIL binders are
 * unit placeholders stored as int64_t). */
static const char *binder_ctype(EmitCtx *ctx, TypeKind ty) {
    /* TY_NEVER (`!`) is the dead placeholder of a diverging computation (a `panic`
     * handler case): declare it as the int64_t nil-placeholder word, never `void`
     * (`type_c_name(TY_NEVER)` -> "void", which yields an illegal `void __t;`
     * local).  atom_ok admits TY_NEVER for the same reason. */
    if (ty == TY_NIL || ty == TY_UNKNOWN || ty == TY_NEVER) return "int64_t";
    return emit_type_c_name(ctx, emit_type_from_kind(ty));
}

/* Like binder_ctype, but uses the full Type when it is a struct / ADT so the
 * local is declared with its real (monomorphized) C type -- e.g.
 * tur_adt_Option__int -- rather than a bare, wrong fallback.  Such a value is a
 * local that never crosses the DK slot (crossing is gated by atom_ok /
 * fn_sig_ok, which reject non-slot types); only scalar fields it yields cross. */
static const char *binder_ctype_full(EmitCtx *ctx, TypeKind ty, const Type *t) {
    /* TY_TYVAR: a monomorph body's generic binder (`A`) -- keep the named Type so
     * emit_type_c_name resolves it through the active spec to its concrete C type
     * (a scalar tyvar reaches this path; binder_ctype would drop the name and
     * mis-spell it).  Only occurs while a monomorph is emitted (spec active). */
    if (t && (ty == TY_STRUCT || ty == TY_ADT || ty == TY_APP || ty == TY_TYVAR))
        return emit_type_c_name(ctx, *t);
    return binder_ctype(ctx, ty);
}

/* Atom types we can materialize as a slot-representable value.
 * TY_NEVER (`!`, the bottom type) is admitted like TY_NIL: it only arises as the
 * DEAD result placeholder of a diverging computation -- e.g. a handler case that
 * ends in `(panic ...)` yields a `!`-typed word that the CPS translation still
 * threads to the prompt, but the panic aborts before that delivery is reached.
 * The placeholder rides the slot as the nil word (0), so crossing it is sound;
 * without this a `(panic ...)`-terminated handler evicts the whole function
 * (e.g. `with-abort-panic` in effect-abort -> main + safe-divide off CPS). */
static bool atom_ok(const CAtom *a) {
    switch (a->kind) {
        case CA_INT:  return slot_ty(a->ty) || a->ty == TY_UNKNOWN;  /* any <=64-bit int width */
        case CA_BOOL: return true;
        case CA_UNIT: return true;   /* nil placeholder (0) */
        case CA_STR:  return true;   /* cstr literal (Tier A pointer) */
        case CA_FLOAT: return true;  /* float/double literal (Tier B) */
        case CA_VAR:
            /* A poly-fat closure (`tur_poly_fn_t`) is a multi-word aggregate that
             * cannot ride the one-word slot / a `void*` temp: spilling it (`t2 =
             * f;`) or crossing it into a slot miscompiles.  Calling such a param
             * goes through a CT_LETRAW delegation (collect_free_vars, not atom_ok),
             * so rejecting it here only blocks the unsound slot-crossing use and
             * evicts a function that threads a poly-fn value as a plain value. */
            if (a->var && a->var->is_poly_fn) return false;
            /* E2a-pass (cps-tramp-resume): a CAPTURELESS (non-poly) fn-value is a bare
             * int64 fn-ptr -- a scalar that rides the slot -- so admit it as an atom
             * (e.g. passing a lambda into a HOF-call inside a DK handler's delim).
             * Gated: flag-off keeps the historical rejection byte-identical. */
            if (a->ty == TY_FN && !(a->var && a->var->is_poly_fn))
                return true;
            return slot_ok_t(a->type, a->ty) || a->ty == TY_NIL || a->ty == TY_NEVER;
        case CA_CVAR:
            if (a->ty == TY_FN) return true;
            return slot_ok_t(a->type, a->ty) || a->ty == TY_NIL || a->ty == TY_NEVER;
        default:      return false;  /* CA_OTHER */
    }
}

static bool is_println_shape(BuiltinShape s) {
    return s == BS_PRINTLN_INT || s == BS_PRINTLN_BOOL || s == BS_PRINTLN_UINT
        || s == BS_PRINTLN_CSTR || s == BS_PRINTLN_FLOAT || s == BS_PRINTLN_FLOAT32;
}

/* The synthetic effect that a cross-function `shift` desugars onto (see
 * elab_effects.c `wrap_reset_body_with_shift_handler`).  A `(perform (__Shift
 * recv))` carries a shift RECEIVER value; the __Shift handler case invokes it as
 * `(recv k)` after bridge-wrapping the DK continuation.  Recognized by interned
 * name so the receiver-admission / bridge-wrap relaxations stay strictly scoped
 * to __Shift and never touch ordinary user effects. */
static bool is_shift_effect(const Symbol *eff) {
    return eff && eff->name && strcmp(eff->name, "__Shift") == 0;
}

/* A __Shift receiver atom: a capture-free `TY_FN` value passed as the single
 * argument of `(perform (__Shift recv))`.  Admitted ONLY at the __Shift perform
 * site (never via the general atom_ok, which a global TY_FN widening would
 * miscompile).  A CAPTURING receiver never reaches here as a clean atom -- it
 * bails at CPS translation (EX_CLOSURE -> CT_UNSUPPORTED), so a TY_FN atom here
 * is always the lifted, capture-free receiver the direct emitter materializes as
 * a plain function pointer that rides the one-word effect slot. */
static bool shift_recv_atom_ok(const CAtom *a) {
    return (a->kind == CA_VAR || a->kind == CA_CVAR) && a->ty == TY_FN;
}

static bool shape_supported(const BuiltinSpec *sp) {
    if (!sp) return false;
    switch (sp->shape) {
        case BS_BIN_INFIX:
        case BS_VARIADIC_FOLD:
        case BS_DIV_CHECK:
        case BS_PREFIX_UNARY:
        case BS_AND_SC:
        case BS_OR_SC:
        case BS_FUNC_CALL:   /* a plain C runtime call c_op(args); prim_expr emits it */
        case BS_PRINTLN_INT:
        case BS_PRINTLN_BOOL:
        case BS_PRINTLN_UINT:
        case BS_PRINTLN_CSTR:
        case BS_PRINTLN_FLOAT:
        case BS_PRINTLN_FLOAT32:
            return true;
        default:
            return false;
    }
}

/* Measurement helper (TUR_TRACE_EVICT readiness gate): find the first
 * CT_UNSUPPORTED node in a translated function, so the eviction trace can name
 * the residual form.  Not part of the admission logic (term_core_ok owns that);
 * this only walks the tree to surface a `why` for the trace. */
static const CTerm *first_unsupported(const CTerm *t) {
    if (!t) return NULL;
    const CTerm *r = NULL;
    switch (t->kind) {
        case CT_UNSUPPORTED: return t;
        case CT_LETVAL:   return first_unsupported(t->as.letval.body);
        case CT_LETPRIM:  return first_unsupported(t->as.letprim.body);
        case CT_LETCALL:  return first_unsupported(t->as.letcall.body);
        case CT_LETRAW:   return first_unsupported(t->as.letraw.body);
        case CT_LETCONT:
            r = first_unsupported(t->as.letcont.jbody);
            return r ? r : first_unsupported(t->as.letcont.body);
        case CT_IF:
            r = first_unsupported(t->as.if_.then_);
            return r ? r : first_unsupported(t->as.if_.else_);
        case CT_MATCH:
            for (uint32_t i = 0; i < t->as.match.n_arms; i++) {
                r = first_unsupported(t->as.match.arms[i].body);
                if (r) return r;
            }
            return NULL;
        case CT_RESET:
            r = first_unsupported(t->as.reset.delim);
            return r ? r : first_unsupported(t->as.reset.body);
        case CT_SHIFT:    return first_unsupported(t->as.shift.body);
        case CT_HANDLE:
            r = first_unsupported(t->as.handle.delim);
            if (r) return r;
            for (uint32_t i = 0; i < t->as.handle.n_cases; i++) {
                r = first_unsupported(t->as.handle.cases[i].case_body);
                if (r) return r;
            }
            return first_unsupported(t->as.handle.body);
        case CT_PERFORM:  return first_unsupported(t->as.perform.body);
        case CT_RESUME:   return first_unsupported(t->as.resume.body);
        case CT_CLONEABLE: return first_unsupported(t->as.cloneable.body);
        case CT_CALLCC:   return first_unsupported(t->as.callcc.body);
        case CT_LOOP:     return first_unsupported(t->as.loop.body);
        default: return NULL;   /* CT_APPCONT / CT_TAILCALL / CT_CONTINUE: leaves */
    }
}

/* serial-receiver-effect-cannot-reach-enclosing-handler: every outward serial
 * receiver in `t` is CPS-emitted (the reset calls its `__cps` twin).  Read by
 * the classification fixpoint, once every entry exists: a function whose
 * outward receiver is not in S is evicted with it, so it falls back to the
 * direct emitter's TUR-E0706 rather than calling a twin that was never
 * emitted.  Same traversal as first_unsupported. */
static bool binding_cps_reachable(const Binding *b);
static bool outward_receivers_in_s(const CTerm *t) {
    if (!t) return true;
    switch (t->kind) {
        case CT_LETVAL:   return outward_receivers_in_s(t->as.letval.body);
        case CT_LETPRIM:  return outward_receivers_in_s(t->as.letprim.body);
        case CT_LETCALL:  return outward_receivers_in_s(t->as.letcall.body);
        case CT_LETRAW:   return outward_receivers_in_s(t->as.letraw.body);
        case CT_LETCONT:
            return outward_receivers_in_s(t->as.letcont.jbody) &&
                   outward_receivers_in_s(t->as.letcont.body);
        case CT_IF:
            return outward_receivers_in_s(t->as.if_.then_) &&
                   outward_receivers_in_s(t->as.if_.else_);
        case CT_MATCH:
            for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                if (!outward_receivers_in_s(t->as.match.arms[i].body)) return false;
            return true;
        case CT_RESET:
            return outward_receivers_in_s(t->as.reset.delim) &&
                   outward_receivers_in_s(t->as.reset.body);
        case CT_SHIFT:    return outward_receivers_in_s(t->as.shift.body);
        case CT_HANDLE:
            if (!outward_receivers_in_s(t->as.handle.delim)) return false;
            for (uint32_t i = 0; i < t->as.handle.n_cases; i++)
                if (!outward_receivers_in_s(t->as.handle.cases[i].case_body)) return false;
            return outward_receivers_in_s(t->as.handle.body);
        case CT_PERFORM:  return outward_receivers_in_s(t->as.perform.body);
        case CT_RESUME:   return outward_receivers_in_s(t->as.resume.body);
        case CT_CLONEABLE:
            if (t->as.cloneable.recv_outward) {
                /* A closure receiver (shape 2 of serial-receiver-effect-under-
                 * if-closure-or-leaf) is called through its lifted lambda's
                 * twin, so that lambda is what must be in S. */
                const Binding *rb = t->as.cloneable.receiver;
                const Expr *rx = t->as.cloneable.receiver_expr;
                if (!rb && rx && rx->kind == EX_CLOSURE && rx->as.closure_.closure &&
                    rx->as.closure_.closure->fn)
                    rb = rx->as.closure_.closure->fn->binding;
                if (!(rb && binding_cps_reachable(rb))) return false;
            }
            return outward_receivers_in_s(t->as.cloneable.body);
        case CT_CALLCC:   return outward_receivers_in_s(t->as.callcc.body);
        case CT_LOOP:     return outward_receivers_in_s(t->as.loop.body);
        default: return true;
    }
}

static bool term_core_ok(const CTerm *t);
static bool delim_ok(const CTerm *t);   /* reset-delim admission (permits KK_PROMPT delivery) */
static bool handle_delim_ok(const CTerm *t);  /* top-level handle-body admission (join-to-prompt, no interior control op) */
static bool letraw_ok(const CTerm *t);  /* CT_LETRAW soundness (owning-drop guard) */
static bool letraw_effect_free(const CTerm *t);  /* CT_LETRAW performs no fiber effect */
static const FnDef *fd_for_binding(const Expr *program, const Binding *b);
static const Expr *g_prog;   /* fwd (defined below): program the classifier is keyed on */
static void ptc_walk(const Expr *e, const Binding *p, bool tail,
                     int *val, int *tailc, int *ntc);  /* fwd: param value/callee-use walk */
static bool binding_in_s(const Binding *b);  /* callee CPS-emitted? (cps->cps vs cps->direct) */
static bool binding_cps_reachable(const Binding *b);  /* in_s OR mono-template */

/* A poly-fat closure (`tur_poly_fn_t`) is a multi-word aggregate.  When such a
 * value is THREADED as a call argument the CT-IR spills it to an intermediate
 * one-word slot / `void*` temp first (`t2 = f;`), which cannot hold the fat
 * struct -- a hard C error (`incompatible types ... from 'tur_poly_fn_t'`).  A
 * function that only CALLS its poly-fn param (a delegated indirect call, never
 * re-threading it as an arg) is fine, so this is an arg-position check, not a
 * signature reject: fn_sig_ok still admits a poly-fn PARAM. */
static bool atom_is_fat_fn(const CAtom *a) {
    return a->kind == CA_VAR && a->var && a->var->is_poly_fn;
}

/* A fn-VALUE atom (a plain `TY_FN` closure/fn-pointer, or a poly-fat closure).
 * Threading one into a call argument is admitted only when the CALLEE is provably
 * effect-free (callee_effect_free), so this just recognizes the atom shape. */
static bool atom_is_fn_value(const CAtom *a) {
    if (atom_is_fat_fn(a)) return true;
    return (a->kind == CA_VAR || a->kind == CA_CVAR) && a->ty == TY_FN;
}

/* Is the statically-known callee provably effect-free -- its DECLARED effect row
 * is empty (NULL / ERK_EMPTY; a row VARIABLE or any concrete effect reads as
 * non-empty and is rejected, so this is conservative and fixpoint-independent --
 * the row is set at elaboration, not by the taint pass)?  An effect-free callee
 * cannot perform an effect THROUGH a fn-value argument: were the callback
 * effectful and invoked, that effect would propagate into the callee's own
 * signature (row polymorphism -- `apply-cb` calling `f : (fn [] #fx{Ask} int)` is
 * itself `#fx{Ask}`).  So threading ANY fn value into an effect-free callee can
 * never create the fiber<->DK effect mismatch that keeps an EFFECTFUL callback on
 * the fiber path (the `unhandled effect` hazard in the handle_delim_ok NOTE).
 * An indirect / unknown callee (fn == NULL) is conservatively NOT effect-free. */
static bool callee_effect_free(const Binding *fn) {
    if (!fn || fn->type.kind != TY_FN) return false;
    /* Runtime-pure, not merely empty: a capability tag (IO from a `println`,
     * FS, Bt) is never performed, so it cannot cross a fn-value argument. */
    if (effect_row_is_runtime_pure(fn->type.as.fn.effect_row)) return true;
    /* A ROW-VARIABLE declared row (`#fx{e}`) reads as non-empty, but the fn may be
     * runtime-PURE: its INFERRED row (computed over the body by the P19-2 effect
     * pass, stable before codegen so still fixpoint-independent) is the sound
     * summary of what it ACTUALLY performs.  A row-variable fn that invokes no
     * effectful callback has an EMPTY inferred row -> genuinely effect-free (e.g.
     * `run-twice [x] #fx{e} = (+ x x)`).  Fall back to it when the declared row is
     * a non-empty row variable. */
    const FnDef *fd = g_prog ? fd_for_binding(g_prog, fn) : NULL;
    if (fd && fd->inferred_effect_row && effect_row_is_runtime_pure(fd->inferred_effect_row))
        return true;
    return false;
}

/* Whether an atom may cross into a call ARGUMENT slot.
 *
 *  - A poly-fat closure never can (spilling it to a temp slot miscompiles),
 *    regardless of whether the call is cps->cps or cps->direct.  (A fn value
 *    threaded into an EFFECT-FREE callee is admitted separately at the call site
 *    via callee_effect_free, before this predicate runs.)
 *  - A Tier C by-value-aggregate atom (slot_box_ty, e.g. `tur_adt_Option__int`)
 *    can cross a cps->cps call (the callee's `__cps` params match the CPS ABI)
 *    but NOT a cps->DIRECT call: there the delegated call emits the arg RAW to
 *    the callee's direct-emitter C signature, which takes a by-value ADT through
 *    the CARRIER ABI (int64_t) -- so a bare-struct arg where an int64 carrier is
 *    expected is a hard C error.  The direct emitter boxes such an arg
 *    (emit_byvalue_carrier_abi); the CPS delegation does not, so the function
 *    must EVICT.  Scalars and heap-handle carriers already ARE the int64 carrier
 *    and cross either way. */
static bool call_arg_ok(const CAtom *a, bool cps_to_direct) {
    if (!atom_ok(a)) return false;
    if (atom_is_fat_fn(a)) return false;
    /* The reject exists because a by-value ADT arg reaching an UNCOLORED callee
     * is emitted raw against that callee's direct-emitter C signature, which
     * takes the aggregate through the carrier ABI -- a raw-struct-for-int64 arg.
     *
     * perform-in-fn-with-any-param-has-no-cps-lowering: `any` is the one member
     * of slot_box_ty for which those two spellings AGREE.  The direct emitter
     * declares such a parameter `tur_tagged_t` by value (type_struct_pass_by_ptr
     * says so) and emit_params spells the CPS side the same way, so the raw arg
     * is exactly what the callee's signature wants.  Without this, admitting
     * `any` to slot_box_ty at all would have made every cps->direct call taking
     * one evict -- which is what `main` did: it passes an `any` to the very
     * function whose `perform` this change exists to lower. */
    if (cps_to_direct && (a->kind == CA_VAR || a->kind == CA_CVAR)
        && slot_box_ty(a->type)
        && !(a->type && a->type->kind == TY_ANY))
        return false;
    return true;
}

/* Every arg of a call to `fn` admissible as a call arg?  A fn-VALUE arg is
 * admitted when `fn` is effect-free (callee_effect_free) -- pure higher-order
 * value threading (option-eq? / hamt map+filter / parser combinators passing a
 * comparator/mapper); every other arg takes the ordinary call_arg_ok gate. */
static EmitCtx *g_emit_ctx;   /* defined below (G3b) */
static const FnDef *cps_fd_of_binding(const Binding *b);   /* defined beside ent_of_binding */
/* colored-call-inside-match-evicts-the-cps-backend, shape 2: the cps->direct
 * reject in call_arg_ok exists for a callee whose direct C signature takes a
 * by-value ADT through the int64 CARRIER (a generic base's `(Option A)`), where
 * a raw struct arg is a hard C error.  A NON-generic callee declares the
 * aggregate itself -- `static int64_t wrap_val(tur_adt_Wrap w)` -- and the
 * CPS emitter's typed arg renderer (atoms_csv_call_typed) already passes a
 * by-value aggregate atom raw, which is exactly that signature.  So `(wrap-val
 * (handle (mk-wrap) ...))` in a colored `main` need not evict: admit the atom
 * when the callee's declared parameter spells the SAME aggregate C type as the
 * atom, narrow (not wide -- a wide by-value ADT param is passed by pointer on
 * the direct path, which the raw emission would not match). */
static bool cps_direct_param_takes_agg_raw(const Binding *fn, uint32_t i,
                                           const CAtom *a) {
    if (!g_emit_ctx || !fn || fn->type.kind != TY_FN || !a || !a->type) return false;
    if (i >= fn->type.as.fn.arity) return false;
    if (fn->closure_fn_binding) return false;
    const Type *pt = NULL;
    const FnDef *fd = cps_fd_of_binding(fn);
    if (fd && i < fd->n_params && fd->params[i]) {
        if (fd->params[i]->is_poly_fn) return false;
        pt = &fd->params[i]->type;
    } else if (fn->type.as.fn.arg_full_types) {
        pt = fn->type.as.fn.arg_full_types[i];
    }
    if (!pt) return false;
    if (pt->kind != a->type->kind) return false;
    if (!type_eq(*pt, *a->type)) return false;
    if (type_struct_pass_by_ptr(*a->type)) return false;   /* direct path takes `const T *` */
    const char *pc = binder_ctype_full(g_emit_ctx, pt->kind, pt);
    const char *ac = binder_ctype_full(g_emit_ctx, a->ty, a->type);
    if (!pc || !ac || strcmp(pc, ac) != 0) return false;
    if (strcmp(pc, "int64_t") == 0 || strchr(pc, '*')) return false;
    return true;
}

static bool call_args_ok(const Binding *fn, const CAtom *args, uint32_t n,
                         bool cps_to_direct) {
    bool cef = callee_effect_free(fn);
    for (uint32_t i = 0; i < n; i++) {
        /* Pure higher-order value threading into an effect-free callee is admitted
         * for a SCALAR fn value (a bare fn-ptr / closure carrier that crosses the
         * arg slot as one word).  A FAT closure (`tur_poly_fn_t`, is_poly_fn) is a
         * multi-word aggregate that CANNOT cross the one-word arg slot (the CT-IR
         * spills it to a `void *` temp -- a hard C error), so it is NOT admitted by
         * this fast path even into an effect-free callee: it falls to call_arg_ok,
         * whose atom_is_fat_fn reject evicts the caller (E2b keeps a fat-fn param
         * that is only CALLED/CAPTURED, never re-threaded as an arg). */
        if (cef && atom_is_fn_value(&args[i]) && !atom_is_fat_fn(&args[i])) continue;
        if (cps_to_direct && !call_arg_ok(&args[i], true) &&
            call_arg_ok(&args[i], false) &&
            cps_direct_param_takes_agg_raw(fn, i, &args[i]))
            continue;
        if (!call_arg_ok(&args[i], cps_to_direct)) return false;
    }
    return true;
}

/* ---- C3 delimited-control subset predicates -------------------------- *
 * The C3 backend lowers a reset/shift only in a restricted, zero-capture,
 * identity-receiver subset; anything else keeps its direct-style emission.
 * These predicates gate that subset. */

#define CC_MAX_BOUND 128

/* Source bindings that are NOT captures inside the lifted body being checked
 * (an effect handler case's params and continuation `k`, bound by the DKHandler
 * signature).  Set by has_capture() before each walk. */
static const Binding *g_excl_b[8];
static int            g_excl_n;

/* True while collecting captures for a SINGLE-SHOT continuation (a reset / handle
 * / perform continuation, or a shift body).  In the admitted subset a
 * continuation `k` is resumed AT MOST ONCE (a second resume is a hard error,
 * TUR-E0201; multi-shot needs cloneable-shift, which falls back), so such a
 * continuation runs at most once.  That makes it safe to capture an OWNING value
 * (an rc handle, a drop-glue aggregate, a heap handle) by a shallow value copy
 * with no clone: the value's drop -- which the drop-insertion pass sinks into the
 * continuation -- then runs exactly once, balanced, no double-free.  A handler
 * CASE body is NOT single-shot (it runs once per perform, potentially many times
 * in a loop), so this stays false for collect_caps_case and owning captures there
 * still bail. */
static bool g_cap_single_shot;

static bool binding_excluded(const Binding *b) {
    for (int i = 0; i < g_excl_n; i++) if (g_excl_b[i] == b) return true;
    return false;
}

/* True if `b` is one of a CT_CLONEABLE node's own `let` prelude bindings -- a C
 * local emitted at the reset site, hence not a capture of any enclosing frame. */
static bool cloneable_is_local(const CTerm *t, const Binding *b) {
    for (uint32_t i = 0; i < t->as.cloneable.n_lets; i++)
        if (t->as.cloneable.lets[i].binding == b) return true;
    return false;
}

/* The free variables of a CT_CLONEABLE node's direct-emitted context sub-exprs
 * (each `let` init, and -- if present -- the `if` condition and pure arm), with
 * the node's own `let` bindings excluded (they are locals at the reset site).
 * Returns a malloc'd array the caller frees; *n_out is the count (0 => NULL).
 * The same complete free-var analysis CT_LETRAW uses, so a capturing context
 * sub-expr surfaces its enclosing locals for the lifted-helper env. */
static Binding **cloneable_ctx_free_vars(const CTerm *t, uint32_t *n_out) {
    *n_out = 0;
    Binding *locals[16];
    uint8_t n_locals = 0;
    for (uint32_t i = 0; i < t->as.cloneable.n_lets && n_locals < 16; i++)
        if (t->as.cloneable.lets[i].binding)
            locals[n_locals++] = (Binding *)t->as.cloneable.lets[i].binding;

    Binding **acc = NULL;
    uint32_t n_acc = 0, cap = 0;
    #define CLONE_ADD_FV(ex) do { const Expr *_e = (ex); \
        if (_e) { uint32_t _nf = 0; \
            Binding **_fv = collect_free_vars(_e, locals, n_locals, NULL, 0, &_nf); \
            for (uint32_t _i = 0; _i < _nf; _i++) { \
                if (n_acc == cap) { cap = cap ? cap * 2 : 8; \
                    acc = realloc(acc, cap * sizeof(Binding *)); } \
                acc[n_acc++] = _fv[_i]; } \
            free(_fv); } } while (0)
    for (uint32_t i = 0; i < t->as.cloneable.n_lets; i++)
        CLONE_ADD_FV(t->as.cloneable.lets[i].init);
    CLONE_ADD_FV(t->as.cloneable.if_cond);
    CLONE_ADD_FV(t->as.cloneable.if_pure);
    #undef CLONE_ADD_FV
    *n_out = n_acc;
    return acc;
}

/* True if `t` references a free variable that a lifted continuation / shift /
 * handler-case helper would have to capture: a non-global, non-excluded source
 * var, or a CPS var not bound within `t` and not the excluded incoming value
 * `exclude` (UINT32_MAX = none).  The zero-capture cut falls back when it holds. */
static bool has_capture_rec(const CTerm *t, uint32_t exclude,
                            uint32_t *bound, int nb) {
    if (!t || nb >= CC_MAX_BOUND) return t ? true : false;
    #define CC_ATOM(a) do { const CAtom *_a = (a); \
        if (_a->kind == CA_VAR) { \
            if (_a->var && !_a->var->is_global && !binding_excluded(_a->var)) { \
                /* a source binding's cvar id is its binding id; the excluded */ \
                /* incoming value and locally-bound names are not captures.   */ \
                uint32_t _id = _a->var->id; bool _f = (_id != exclude); \
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; } \
                if (_f) return true; } } \
        else if (_a->kind == CA_CVAR) { \
            if (_a->cvar_id != exclude) { bool _f = true; \
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _a->cvar_id) { _f = false; break; } \
                if (_f) return true; } } } while (0)
    switch (t->kind) {
        case CT_APPCONT: CC_ATOM(&t->as.appcont.v); return false;
        case CT_LETVAL:
            CC_ATOM(&t->as.letval.v);
            bound[nb] = t->as.letval.x.id;
            return has_capture_rec(t->as.letval.body, exclude, bound, nb + 1);
        case CT_LETPRIM:
            for (uint32_t i = 0; i < t->as.letprim.n; i++) CC_ATOM(&t->as.letprim.args[i]);
            bound[nb] = t->as.letprim.x.id;
            return has_capture_rec(t->as.letprim.body, exclude, bound, nb + 1);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++) CC_ATOM(&t->as.letcall.args[i]);
            bound[nb] = t->as.letcall.x.id;
            return has_capture_rec(t->as.letcall.body, exclude, bound, nb + 1);
        case CT_TAILCALL:
            for (uint32_t i = 0; i < t->as.tailcall.n; i++) CC_ATOM(&t->as.tailcall.args[i]);
            /* E2c field-load callee (fn == NULL): its atom is read like an
             * argument -- a join continuation that calls it must capture it
             * (`(.run (make-struct FE fe) <arg with a CPS call>)` read an
             * uncaptured `__t2` in the join). */
            if (t->as.tailcall.via_registry && !t->as.tailcall.fn)
                CC_ATOM(&t->as.tailcall.fn_atom);
            /* E2c: a `via_registry` tailcall's fn-value callee is a capture when it
             * is an enclosing (non-local, non-global) param -- mirrors the
             * collect_caps_rec CT_TAILCALL case. */
            if (t->as.tailcall.via_registry || t->as.tailcall.via_fncps) {
                const Binding *fn = t->as.tailcall.fn;
                if (fn && !fn->is_global && !binding_excluded(fn)) {
                    uint32_t _id = fn->id; bool _f = (_id != exclude);
                    for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                    if (_f) return true;
                }
            }
            return false;
        case CT_IF:
            CC_ATOM(&t->as.if_.cond);
            return has_capture_rec(t->as.if_.then_, exclude, bound, nb)
                || has_capture_rec(t->as.if_.else_, exclude, bound, nb);
        case CT_MATCH:
            CC_ATOM(&t->as.match.scrut);
            for (uint32_t ai = 0; ai < t->as.match.n_arms; ai++) {
                const CMatchArm *arm = &t->as.match.arms[ai];
                uint32_t nnb = nb;
                for (uint32_t bi = 0; bi < arm->n_fields && nnb < CC_MAX_BOUND; bi++)
                    if (arm->fields[bi]) bound[nnb++] = arm->fields[bi]->id;
                if (has_capture_rec(arm->body, exclude, bound, nnb)) return true;
            }
            return false;
        case CT_LETCONT:
            bound[nb] = t->as.letcont.param.id;
            return has_capture_rec(t->as.letcont.jbody, exclude, bound, nb + 1)
                || has_capture_rec(t->as.letcont.body, exclude, bound, nb + 1);
        case CT_RESUME:
            CC_ATOM(&t->as.resume.k);   /* k is the handler's excluded continuation */
            CC_ATOM(&t->as.resume.v);
            bound[nb] = t->as.resume.x.id;
            return has_capture_rec(t->as.resume.body, exclude, bound, nb + 1);
        case CT_CLONEABLE: {
            /* receiver is a global fn -- no capture; Shape 2 frame operands may
             * be captured vars (they ride the frame env), except those naming the
             * node's own `let` prelude locals. */
            for (uint32_t _i = 0; _i < t->as.cloneable.n_frames; _i++) {
                const CAtom *_op = &t->as.cloneable.frames[_i].operand;
                if (_op->kind == CA_VAR && cloneable_is_local(t, _op->var)) continue;
                CC_ATOM(_op);
            }
            /* Direct-emitted context sub-exprs (let inits, if cond/pure arm): a
             * non-local free var they reference is a capture too. */
            uint32_t _nfv = 0;
            Binding **_fv = cloneable_ctx_free_vars(t, &_nfv);
            for (uint32_t _i = 0; _i < _nfv; _i++) {
                const Binding *_bb = _fv[_i];
                if (!_bb || _bb->is_global || binding_excluded(_bb)) continue;
                uint32_t _id = _bb->id; bool _f = (_id != exclude);
                for (int _j = 0; _j < nb; _j++) if (bound[_j] == _id) { _f = false; break; }
                if (_f) { free(_fv); return true; }
            }
            free(_fv);
            /* U7: a closure receiver's free vars are captures too. */
            if (t->as.cloneable.receiver_expr) {
                uint32_t _rn = 0;
                Binding **_rfv = collect_free_vars(t->as.cloneable.receiver_expr, NULL, 0, NULL, 0, &_rn);
                for (uint32_t _i = 0; _i < _rn; _i++) {
                    const Binding *_bb = _rfv[_i];
                    if (_bb && !_bb->is_global && !binding_excluded(_bb)) {
                        uint32_t _id = _bb->id; bool _f = (_id != exclude);
                        for (int _j = 0; _j < nb; _j++) if (bound[_j] == _id) { _f = false; break; }
                        if (_f) { free(_rfv); return true; }
                    }
                }
                free(_rfv);
            }
            bound[nb] = t->as.cloneable.x.id;
            return has_capture_rec(t->as.cloneable.body, exclude, bound, nb + 1);
        }
        case CT_LETRAW: {
            /* The complete set of enclosing names the delegated Expr references is
             * exactly its free-variable set -- the same complete walker
             * collect_caps_rec uses.  A non-atomic operand (O1-a: e.g.
             * `(rc/of (+ a b))`) references locals nested BELOW a top-level var, so
             * walk the whole operand rather than only direct-var operands;
             * otherwise a real capture would be missed and the term wrongly
             * admitted into a lifted zero-capture body (an undeclared C name).  A
             * composite the walker cannot analyze surfaces no free vars, matching
             * the collect_caps_rec / cap_add fallback path. */
            const Expr *le = t->as.letraw.e;
            uint32_t n_fv = 0;
            Binding **fv = collect_free_vars(le, NULL, 0, NULL, 0, &n_fv);
            for (uint32_t i = 0; i < n_fv; i++) {
                const Binding *b = fv[i];
                if (!b || b->is_global || binding_excluded(b)) continue;
                uint32_t _id = b->id; bool _f = (_id != exclude);
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                if (_f) { free(fv); return true; }
            }
            free(fv);
            bound[nb] = t->as.letraw.x.id;
            return has_capture_rec(t->as.letraw.body, exclude, bound, nb + 1);
        }
        case CT_CALLCC: {
            /* A capturing receiver makes the escape landing capture its free vars;
             * surface them (collect_free_vars descends into the EX_CALLCC receiver)
             * and report a capture so the zero-capture join cut sees it.  Then bind
             * x and recurse the body. */
            uint32_t n_fv = 0;
            Binding **fv = collect_free_vars(t->as.callcc.e, NULL, 0, NULL, 0, &n_fv);
            for (uint32_t i = 0; i < n_fv; i++) {
                const Binding *cb = fv[i];
                if (cb && !cb->is_global && !binding_excluded(cb)) {
                    uint32_t _id = cb->id; bool _f = (_id != exclude);
                    for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                    if (_f) { free(fv); return true; }
                }
            }
            free(fv);
            bound[nb] = t->as.callcc.x.id;
            return has_capture_rec(t->as.callcc.body, exclude, bound, nb + 1);
        }
        case CT_CONTINUE:   /* cps-while-native: back-edge args are captures like a tailcall's */
            for (uint32_t i = 0; i < t->as.cont_.n; i++) CC_ATOM(&t->as.cont_.args[i]);
            return false;
        case CT_LOOP: {
            /* cps-while-native: a CT_LOOP in a lifted continuation (e.g. a while
             * loop in a handle's continuation) captures its init atoms and the
             * outer vars its body reads; the loop-carried params are bound. */
            for (uint32_t i = 0; i < t->as.loop.n_params; i++) CC_ATOM(&t->as.loop.inits[i]);
            uint32_t nnb = nb;
            for (uint32_t i = 0; i < t->as.loop.n_params && nnb < CC_MAX_BOUND; i++)
                if (t->as.loop.params[i].bind) bound[nnb++] = t->as.loop.params[i].bind->id;
            return has_capture_rec(t->as.loop.body, exclude, bound, nnb);
        }
        default: return true;   /* nested reset/shift/handle/perform in a lifted body: bail */
    }
    #undef CC_ATOM
}

__attribute__((unused))
static bool has_capture(const CTerm *t, uint32_t exclude) {
    uint32_t bound[CC_MAX_BOUND];
    g_excl_n = 0;
    return has_capture_rec(t, exclude, bound, 0);
}

/* ---- N6.3: capture collection for a lifted perform continuation ---------- *
 * A perform continuation that references enclosing locals is lifted with a real
 * env (a heap struct of the captured values), not the zero-capture shortcut.
 * This collector gathers the distinct captured SOURCE bindings a body needs,
 * and is exhaustive-or-bails: it collects a scalar (Copy, multi-shot-safe)
 * CA_VAR capture, and sets `ok=false` for anything it cannot represent in an env
 * (a captured CPS var, a non-scalar capture, a delegated composite with a
 * capture, or a nested control op).  A false `ok` means the caller keeps the
 * zero-capture fallback.  Mirrors has_capture_rec's traversal exactly so no
 * capture is missed (a missed capture would emit an undeclared reference). */
#define CC_MAX_CAPS 16
/* A capture is either a source binding (`b`, named via name_for_binding) or a
 * CPS-introduced result var (`cvname`, a fresh "tN" name that is both its
 * declaration and its reference).  Exactly one of b / cvname is set. */
typedef struct {
    const Binding *b[CC_MAX_CAPS];
    const char    *cvname[CC_MAX_CAPS];
    uint32_t       cvid[CC_MAX_CAPS];
    TypeKind       ty[CC_MAX_CAPS];
    const Type    *type[CC_MAX_CAPS];   /* full type (Tier C by-value aggregate); may be NULL */
    bool           polyfn[CC_MAX_CAPS]; /* a rank-2 poly fn value -- env field is tur_poly_fn_t */
    bool           owning[CC_MAX_CAPS]; /* E1: an owning capture admitted into a MULTI-SHOT
                                         * continuation -- the lifted helper clones (increfs)
                                         * it on read-out so each invocation owns its own +1,
                                         * balanced by the body's drop.  Only set for the
                                         * multi-shot owning admission (a single-shot owning
                                         * capture rides by a bare shallow copy, owning=false). */
    int n; bool ok;
} CapSet;

/* A capture rides the env by value: a scalar (Tier A/B) or an owning-free
 * by-value aggregate (Tier C, slot_box_ty).  Both are Copy, so a leaked env
 * storing them is multi-shot-safe with no retain/drop.  Owning handles / carrier
 * ADTs are rejected here (their copy would duplicate a refcount) -- they route
 * through cap_owning_ok / clone-on-read-out instead. */
static bool cap_ty_ok(TypeKind ty, const Type *type) {
    return slot_ty(ty) || slot_box_ty(type);
}

/* An owning-carrying BY-VALUE aggregate: a by-value struct/ADT product whose def
 * `needs_drop_glue` (it has an rc/ref/weak field).  slot_box_ty excludes it (that
 * gate is owning-free only), so it never rides the plain Copy capture gate -- it
 * routes through cap_owning_ok. */
static bool owning_byvalue_aggregate(const Type *t) {
    if (!t) return false;
    Type _r; t = cps_resolve_ty(t, &_r);
    if (type_is_heap_adt(*(Type *)t) || type_is_heap_struct(*(Type *)t)) return false;
    if (!type_is_byvalue_adt_product(*(Type *)t)) return false;
    const AdtDef *d = slot_agg_def(t);
    return d && d->needs_drop_glue;
}

/* E3a (owning-cloneable-capture): is this a 2-arg cloneable call frame that
 * CONSUMES its captured owning env (drops it once per call), rather than
 * ^borrow it?  A consuming frame gets dk_frame_owning env_clone glue so each
 * multi-shot resume owns its own copy to drop.  Detected by: a 2-arg call whose
 * callee param is NOT ^borrow and whose captured operand is a consumable owning
 * kind.  Two kinds, distinguished by the emitted glue:
 *   - rc: cloneable_frame_consumes_rc -- env_clone = rc_strong_increment;
 *   - a FLAT heap carrier: cloneable_frame_consumes_carrier -- env_clone =
 *     malloc + shallow header copy (a real deep copy so each resume frees its own
 *     allocation; flat = no owning fields, so a shallow copy shares nothing). */
static bool cloneable_frame_consume_base(const CloneFrame *fr) {
    if (!fr || !fr->call_fn || fr->ignore_value) return false;
    if (fr->call_fn->type.kind != TY_FN || fr->call_fn->type.as.fn.arity != 2)
        return false;
    if (!fr->operand.type) return false;
    int env_idx = fr->hole_left ? 1 : 0;
    return !FN_ARG_FLAG(fr->call_fn->type.as.fn, env_idx, FA_BORROW);
}
static bool cloneable_frame_consumes_rc(const CloneFrame *fr) {
    return cloneable_frame_consume_base(fr) && fr->operand.type->kind == TY_RC;
}
/* The heap ADT of a carrier operand, or NULL. */
static const AdtDef *cloneable_carrier_def(const Type *t) {
    if (!t || !(type_is_heap_adt(*(Type *)t) || type_is_heap_struct(*(Type *)t)))
        return NULL;
    return (t->kind == TY_ADT) ? t->as.adt_.def
         : (t->kind == TY_APP) ? type_adt_app_def((Type *)t) : NULL;
}
static bool clone_field_is_scalar(TypeKind k) {
    return !(k == TY_RC || k == TY_WEAK || k == TY_REF || k == TY_LREF
             || k == TY_ADT || k == TY_STRUCT || k == TY_APP);
}
/* Aggregate clone fields: rc/weak (incref) or plain scalar only -- a by-value
 * aggregate's ref field cannot be per-copy boxed through the by-value call. */
static bool adt_incref_cloneable_fields(const AdtDef *d) {
    if (!d || d->n_ctors == 0) return false;
    const CtorDef *c = d->ctors[0];
    for (uint32_t i = 0; i < c->n_fields; i++)
        if (!(c->fields[i].kind == TY_RC || c->fields[i].kind == TY_WEAK
              || clone_field_is_scalar(c->fields[i].kind)))
            return false;
    return true;
}
/* Heap-handle clone fields: rc/weak (incref), plain scalar, OR `ref<scalar>` /
 * `lref<scalar>` (deep-copied as a fresh box, mirroring drop_glue's `free`). */
static bool adt_heap_cloneable_fields(const AdtDef *d) {
    if (!d || d->n_ctors == 0) return false;
    const CtorDef *c = d->ctors[0];
    for (uint32_t i = 0; i < c->n_fields; i++) {
        TypeKind k = c->fields[i].kind;
        if (k == TY_RC || k == TY_WEAK || clone_field_is_scalar(k)) continue;
        if ((k == TY_REF || k == TY_LREF)
            && clone_field_is_scalar(c->fields[i].inner_kind)) continue;
        return false;
    }
    return true;
}
static bool cloneable_frame_consumes_carrier(const CloneFrame *fr) {
    if (!cloneable_frame_consume_base(fr)) return false;
    return adt_heap_cloneable_fields(cloneable_carrier_def(fr->operand.type));
}
/* A consuming multi-word owning by-value AGGREGATE frame: the aggregate rides the
 * env by ADDRESS (`&o`), the frame passes it BY VALUE so the callee drops its own
 * copy; the env_clone increfs the aggregate's owning fields (via `&o`) per resume
 * to balance that -- no fresh allocation (the env stays `&o`). */
static bool cloneable_frame_consumes_aggregate(const CloneFrame *fr) {
    if (!cloneable_frame_consume_base(fr)) return false;
    const Type *t = fr->operand.type;
    if (!owning_byvalue_aggregate(t)) return false;
    const AdtDef *d = (t->kind == TY_ADT) ? t->as.adt_.def
                    : (t->kind == TY_APP) ? type_adt_app_def((Type *)t) : NULL;
    return adt_incref_cloneable_fields(d);
}

/* An OWNING capture that may ride the env of a genuinely MULTI-SHOT continuation
 * (a handler CASE body).  The env-capture landing is BORROW-ONLY-first (E-borrow):
 * for the reachable borrow-only shape the capture rides by a bare shallow alias
 * (no clone, no drop -- the enclosing fn owns it and drops it once, now via the
 * P2 auto-drop lowering), which collect_caps_case decides via
 * owning_cap_borrow_only.  cap_owning_ok only says "this owning kind is
 * admissible into a multi-shot env":
 *   - `rc<int>` handle (TY_RC): borrow-only -> bare alias; consuming -> incref
 *     (rc_strong_increment, Option A, memory-safe).
 *   - a carrier ADT / heap struct handle: borrow-only -> bare pointer alias;
 *     consuming -> evict (no scalar incref glue -- rides E3's teardown).
 *   - an owning-carrying by-value aggregate: borrow-only -> bare struct copy;
 *     consuming -> evict.
 * A single-shot continuation still admits any owning capture by a bare copy
 * regardless (g_cap_single_shot, in cap_add).  weak<T> stays excluded. */
static bool cap_owning_ok(TypeKind ty, const Type *type) {
    return ty == TY_RC || carrier_handle_ok(type) || owning_byvalue_aggregate(type);
}

static void cap_add(CapSet *cs, const Binding *b, TypeKind ty, const Type *type) {
    /* A rank-2 poly fn value is a fat closure (tur_poly_fn_t, wider than a slot).
     * It is Copy -- a borrowed function value the callee never drops (its owner
     * outlives the call) -- so it rides the env by value like a by-value
     * aggregate, with `tur_poly_fn_t` as the env-field type.  Everything else
     * goes through the scalar / by-value-aggregate gate. */
    bool is_poly = (b && b->is_poly_fn);
    /* A `^borrow` parameter (is_borrow) is a type-system-guaranteed non-consuming
     * value: the callee reads it but never drops or moves it (the caller owns it
     * and outlives the call, including any deferred continuation, which runs
     * inside this function's dynamic extent).  So an owning `^borrow` value (an
     * rc handle, a drop-glue aggregate) that fails the Copy gate above still rides
     * the env by a shallow value copy with NO retain/drop: the shared handle is
     * never released by this function, so no double-free, and the refcount is
     * unchanged from the direct path (multi-shot reads stay read-only). */
    bool borrowed = (b && b->is_borrow);
    /* In a single-shot continuation any value -- including an owning one -- may
     * ride the env by a shallow value copy: the continuation runs at most once,
     * so a drop it performs on the captured value runs at most once (balanced).
     * (A fn value still routes through the poly path for its tur_poly_fn_t env
     * type; a by-value aggregate through slot_box for its unbox.) */
    bool owning_ok = g_cap_single_shot;
    /* E1: an owning capture in a MULTI-SHOT continuation (owning_ok is false) is
     * admitted iff we can emit its clone glue (cap_owning_ok); the lifted helper
     * then clones it on read-out so each invocation owns its own +1.  A poly fn,
     * a `^borrow`, a Copy value, and a single-shot owning capture never need the
     * clone -- they ride by a bare shallow copy (needs_clone stays false). */
    bool needs_clone = false;
    if (!is_poly && !borrowed && !owning_ok && !cap_ty_ok(ty, type)) {
        if (cap_owning_ok(ty, type)) needs_clone = true;
        else { cs->ok = false; return; }
    }
    for (int i = 0; i < cs->n; i++) if (cs->b[i] == b) return;
    if (cs->n >= CC_MAX_CAPS) { cs->ok = false; return; }
    cs->b[cs->n] = b; cs->cvname[cs->n] = NULL; cs->ty[cs->n] = ty;
    cs->type[cs->n] = type; cs->polyfn[cs->n] = is_poly;
    cs->owning[cs->n] = needs_clone; cs->n++;
}

/* The C type of capture slot i: a fat closure is `tur_poly_fn_t`, everything
 * else its scalar / by-value-aggregate binder type. */
static const char *cap_ctype(EmitCtx *ctx, const CapSet *caps, int i) {
    if (caps->polyfn[i]) return "tur_poly_fn_t";
    /* B7: a by-reference mutable rides the env as its (typed) cell POINTER. */
    if (caps->b[i] && is_byref_mut(caps->b[i])) return byref_cell_ptr_ctype(ctx, caps->b[i]);
    /* E2c: a captureless-effectful fn-value param (a via_registry callee carried
     * by cap_add_fn_scalar) rides the env as a bare int64 direct-entry fn-ptr --
     * `binder_ctype_full(TY_FN)` would leak a bad spelling. */
    if (caps->ty[i] == TY_FN) return "int64_t";
    /* fn-value-fat-normalization (effect-row increment): a normalized fn param
     * FORWARDED as an argument was retyped to TY_PTR_VOID by the elab
     * pass-through (so the arg loop does not double-shim it), so its capture
     * atom arrives as TY_PTR_VOID while the C param it loads from is the
     * int64_t carrier.  Spelling the env field `void *` makes the fill an
     * int->pointer assignment (-Wint-conversion).  The binding still says
     * TY_FN; keep the slot the one-word carrier. */
    if (caps->ty[i] == TY_PTR_VOID && caps->b[i] &&
        caps->b[i]->type.kind == TY_FN)
        return "int64_t";
    return binder_ctype_full(ctx, caps->ty[i], caps->type[i]);
}

/* E2c: capture a captureless-effectful fn-value PARAM (a `via_registry` tailcall's
 * callee) as a bare int64 direct-entry fn-ptr scalar, so a lifted continuation
 * frame that threads `f` (`__tur_cps_lookup((intptr_t)f)`) reads it from its env
 * instead of an out-of-scope param.  Only a NON-poly `TY_FN` binding qualifies (a
 * fat closure `tur_poly_fn_t` is the separate fn_cps channel); everything else
 * bails the capture set (cs->ok = false) exactly like cap_add. */
static void cap_add_fn_scalar(CapSet *cs, const Binding *b) {
    if (!b || b->type.kind != TY_FN || b->is_poly_fn) { cs->ok = false; return; }
    for (int i = 0; i < cs->n; i++) if (cs->b[i] == b) return;
    if (cs->n >= CC_MAX_CAPS) { cs->ok = false; return; }
    cs->b[cs->n] = b; cs->cvname[cs->n] = NULL; cs->ty[cs->n] = TY_FN;
    cs->type[cs->n] = &b->type; cs->polyfn[cs->n] = false;
    cs->owning[cs->n] = false; cs->n++;
}

/* cps-evicts-handle-in-operand-positions: the fresh binders a CT_LETRAW of an
 * EX_POLY_WRAP binds (`tur_poly_fn_t __t2`, see letraw_emits_poly_fn) in the
 * function being emitted.  A lifted continuation that captures one sees only
 * the free CVar -- its ptr<void> node type -- and spelled the env field
 * `void *`, so the fill assigned a `tur_poly_fn_t` to it (invalid C): a rank-2
 * argument atomized ahead of a `handle` in the same call.  Filled by
 * emit_binder_decls, reset per function. */
static uint32_t g_polyfn_cvar_ids[256];
static uint32_t g_n_polyfn_cvars;
static bool cvar_is_polyfn(uint32_t id) {
    for (uint32_t i = 0; i < g_n_polyfn_cvars; i++)
        if (g_polyfn_cvar_ids[i] == id) return true;
    return false;
}

static void cap_add_cvar(CapSet *cs, uint32_t id, const char *name, TypeKind ty, const Type *type) {
    bool is_poly = cvar_is_polyfn(id);
    if ((!is_poly && !cap_ty_ok(ty, type)) || !name) { cs->ok = false; return; }
    for (int i = 0; i < cs->n; i++) if (cs->cvname[i] && cs->cvid[i] == id) return;
    if (cs->n >= CC_MAX_CAPS) { cs->ok = false; return; }
    cs->b[cs->n] = NULL; cs->cvname[cs->n] = name; cs->cvid[cs->n] = id;
    cs->ty[cs->n] = ty; cs->type[cs->n] = type; cs->polyfn[cs->n] = is_poly;
    cs->owning[cs->n] = false; cs->n++;
}

/* cps-evicts-handle-in-operand-positions item 4: the E2c FIELD-LOAD callee of a
 * `via_registry` tailcall (`(.run obj)`, atomized into a CVar) captured by a
 * lifted continuation -- the join a `handle` body's non-atomic argument
 * builds before the call.  It is the same int64 direct-entry fn-ptr word
 * cap_add_fn_scalar carries for a fn-value PARAM callee (cap_ctype spells a
 * TY_FN slot `int64_t`), so it rides the env the same way; the generic CVar
 * gate refuses a TY_FN and evicted the whole function (BODY-STRUCT-JOIN). */
static void cap_add_cvar_fn_scalar(CapSet *cs, uint32_t id, const char *name,
                                   const Type *type) {
    if (!name) { cs->ok = false; return; }
    for (int i = 0; i < cs->n; i++) if (cs->cvname[i] && cs->cvid[i] == id) return;
    if (cs->n >= CC_MAX_CAPS) { cs->ok = false; return; }
    cs->b[cs->n] = NULL; cs->cvname[cs->n] = name; cs->cvid[cs->n] = id;
    cs->ty[cs->n] = TY_FN; cs->type[cs->n] = type; cs->polyfn[cs->n] = false;
    cs->owning[cs->n] = false; cs->n++;
}

static void collect_caps_rec(const CTerm *t, uint32_t exclude,
                             uint32_t *bound, int nb, CapSet *cs) {
    if (!cs->ok) return;
    if (!t || nb >= CC_MAX_BOUND) { if (!t) return; cs->ok = false; return; }
    #define COL_ATOM(a) do { const CAtom *_a = (a); \
        if (_a->kind == CA_VAR) { \
            if (_a->var && !_a->var->is_global && !binding_excluded(_a->var)) { \
                uint32_t _id = _a->var->id; bool _f = (_id != exclude); \
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; } \
                if (_f) cap_add(cs, _a->var, _a->ty, _a->type); } } \
        else if (_a->kind == CA_CVAR) { \
            if (_a->cvar_id != exclude) { bool _f = true; \
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _a->cvar_id) { _f = false; break; } \
                if (_f) cap_add_cvar(cs, _a->cvar_id, _a->cvar_name, _a->ty, _a->type); } } } while (0)
    /* A free source var referenced inside a delegated (direct-emitted) Expr: the
     * same test COL_ATOM applies to a CA_VAR, but reaching through an EX_VAR node
     * to its binding.  Its Type carries the monomorphized def (for slot_box_ty). */
    #define COL_VAREXPR(ve) do { const Expr *_e = (ve); \
        while (_e && _e->kind == EX_ASCRIBE) _e = _e->as.ascribe_.inner; \
        if (_e && _e->kind == EX_VAR && _e->as.var.binding \
            && !_e->as.var.binding->is_global \
            && !binding_excluded(_e->as.var.binding)) { \
            const Binding *_b = _e->as.var.binding; \
            uint32_t _id = _b->id; bool _f = (_id != exclude); \
            for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; } \
            if (_f) cap_add(cs, _b, _b->type.kind, &_b->type); } } while (0)
    switch (t->kind) {
        case CT_APPCONT: COL_ATOM(&t->as.appcont.v); return;
        case CT_LETVAL:
            COL_ATOM(&t->as.letval.v);
            bound[nb] = t->as.letval.x.id;
            collect_caps_rec(t->as.letval.body, exclude, bound, nb + 1, cs); return;
        case CT_LETPRIM:
            for (uint32_t i = 0; i < t->as.letprim.n; i++) COL_ATOM(&t->as.letprim.args[i]);
            bound[nb] = t->as.letprim.x.id;
            collect_caps_rec(t->as.letprim.body, exclude, bound, nb + 1, cs); return;
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++) COL_ATOM(&t->as.letcall.args[i]);
            bound[nb] = t->as.letcall.x.id;
            collect_caps_rec(t->as.letcall.body, exclude, bound, nb + 1, cs); return;
        case CT_TAILCALL:
            for (uint32_t i = 0; i < t->as.tailcall.n; i++) COL_ATOM(&t->as.tailcall.args[i]);
            if (t->as.tailcall.via_registry && !t->as.tailcall.fn) {
                const CAtom *_fa = &t->as.tailcall.fn_atom;   /* E2c field-load callee */
                if (_fa->kind == CA_CVAR && _fa->ty == TY_FN) {
                    bool _f = _fa->cvar_id != exclude;
                    for (int _i = 0; _i < nb; _i++) if (bound[_i] == _fa->cvar_id) { _f = false; break; }
                    if (_f) cap_add_cvar_fn_scalar(cs, _fa->cvar_id, _fa->cvar_name, _fa->type);
                } else {
                    COL_ATOM(_fa);
                }
            }
            /* E2c: a `via_registry` tailcall threads its fn-value CALLEE through
             * `__tur_cps_lookup(f)`; when that callee is an enclosing param (not a
             * local of this lifted body), carry it on the frame env as an int64
             * fn-ptr scalar so the emitted lookup resolves `f`. */
            if (t->as.tailcall.via_registry) {
                const Binding *fn = t->as.tailcall.fn;
                if (fn && !fn->is_global && !binding_excluded(fn)) {
                    uint32_t _id = fn->id; bool _f = (_id != exclude);
                    for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                    if (_f) cap_add_fn_scalar(cs, fn);
                }
            }
            /* E2 (fat-closure fn-value threading): a `via_fncps` tailcall reads its
             * fat-closure poly-fn CALLEE `f` (fn_cps / fn / env); when `f` is an
             * enclosing param, carry it on the frame env as a `tur_poly_fn_t` field
             * (cap_add detects is_poly_fn) so the lifted body's `f.fn_cps` resolves. */
            if (t->as.tailcall.via_fncps) {
                const Binding *fn = t->as.tailcall.fn;
                if (fn && !fn->is_global && !binding_excluded(fn)) {
                    uint32_t _id = fn->id; bool _f = (_id != exclude);
                    for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                    if (_f) cap_add(cs, fn, fn->type.kind, &fn->type);
                }
            }
            return;
        case CT_CONTINUE:   /* cps-while-native: back-edge args, like a tailcall */
            for (uint32_t i = 0; i < t->as.cont_.n; i++) COL_ATOM(&t->as.cont_.args[i]);
            return;
        case CT_LOOP: {
            /* cps-while-native: mirror has_capture_rec's CT_LOOP -- collect the
             * init atoms + the outer vars the loop body reads; bind the carried
             * params.  Lets a while loop in a handle continuation collect its
             * captures instead of bailing (cs->ok = false). */
            for (uint32_t i = 0; i < t->as.loop.n_params; i++) COL_ATOM(&t->as.loop.inits[i]);
            uint32_t nnb = nb;
            for (uint32_t i = 0; i < t->as.loop.n_params && nnb < CC_MAX_BOUND; i++)
                if (t->as.loop.params[i].bind) bound[nnb++] = t->as.loop.params[i].bind->id;
            collect_caps_rec(t->as.loop.body, exclude, bound, nnb, cs); return;
        }
        case CT_IF:
            COL_ATOM(&t->as.if_.cond);
            collect_caps_rec(t->as.if_.then_, exclude, bound, nb, cs);
            collect_caps_rec(t->as.if_.else_, exclude, bound, nb, cs); return;
        case CT_MATCH:
            /* The scrutinee atom may be an enclosing capture; each arm's field
             * bindings are locally bound (extracted from the scrutinee at emit),
             * so add them to the bound set before walking the arm body. */
            COL_ATOM(&t->as.match.scrut);
            for (uint32_t ai = 0; ai < t->as.match.n_arms && cs->ok; ai++) {
                const CMatchArm *arm = &t->as.match.arms[ai];
                uint32_t nnb = nb;
                for (uint32_t bi = 0; bi < arm->n_fields && nnb < CC_MAX_BOUND; bi++)
                    if (arm->fields[bi]) bound[nnb++] = arm->fields[bi]->id;
                collect_caps_rec(arm->body, exclude, bound, nnb, cs);
            }
            return;
        case CT_LETCONT:
            bound[nb] = t->as.letcont.param.id;
            collect_caps_rec(t->as.letcont.jbody, exclude, bound, nb + 1, cs);
            collect_caps_rec(t->as.letcont.body, exclude, bound, nb + 1, cs); return;
        case CT_RESUME:
            /* resume k v: k and v are captured/threaded like any operand; the
             * result binder x is bound for the continuation body.  A capturing
             * handler case resumes, so this must be collectable. */
            COL_ATOM(&t->as.resume.k);
            COL_ATOM(&t->as.resume.v);
            bound[nb] = t->as.resume.x.id;
            collect_caps_rec(t->as.resume.body, exclude, bound, nb + 1, cs); return;
        case CT_CLONEABLE: {
            /* The receiver is a named global fn (no capture); Shape 2 frame
             * operands may be captured vars (they ride the frame env), except
             * those naming the node's own `let` prelude locals.  The direct-
             * emitted context sub-exprs (let inits, if cond/pure) contribute their
             * non-local free vars as captures too.  Bind the result binder for the
             * continuation body and collect its captures. */
            for (uint32_t _i = 0; _i < t->as.cloneable.n_frames; _i++) {
                const CAtom *_op = &t->as.cloneable.frames[_i].operand;
                if (_op->kind == CA_VAR && cloneable_is_local(t, _op->var)) continue;
                COL_ATOM(_op);
            }
            uint32_t _nfv = 0;
            Binding **_fv = cloneable_ctx_free_vars(t, &_nfv);
            for (uint32_t _i = 0; _i < _nfv && cs->ok; _i++) {
                const Binding *_bb = _fv[_i];
                if (!_bb || _bb->is_global || binding_excluded(_bb)) continue;
                uint32_t _id = _bb->id; bool _f = (_id != exclude);
                for (int _j = 0; _j < nb; _j++) if (bound[_j] == _id) { _f = false; break; }
                if (_f) cap_add(cs, _bb, _bb->type.kind, &_bb->type);
            }
            free(_fv);
            /* U7: a closure receiver (receiver_expr) contributes its own captures
             * -- collect_free_vars surfaces the closure's free vars so each scalar
             * capture rides the lifted env (non-Copy bails to fallback). */
            if (t->as.cloneable.receiver_expr) {
                uint32_t _rn = 0;
                Binding **_rfv = collect_free_vars(t->as.cloneable.receiver_expr, NULL, 0, NULL, 0, &_rn);
                for (uint32_t _i = 0; _i < _rn && cs->ok; _i++) {
                    const Binding *_bb = _rfv[_i];
                    if (!_bb || _bb->is_global || binding_excluded(_bb)) continue;
                    uint32_t _id = _bb->id; bool _f = (_id != exclude);
                    for (int _j = 0; _j < nb; _j++) if (bound[_j] == _id) { _f = false; break; }
                    if (_f) cap_add(cs, _bb, _bb->type.kind, &_bb->type);
                }
                free(_rfv);
            }
            bound[nb] = t->as.cloneable.x.id;
            collect_caps_rec(t->as.cloneable.body, exclude, bound, nb + 1, cs); return;
        }
        case CT_LETRAW: {
            /* A delegated (direct-emitted) owning-value op.  Its operand vars are
             * the only free names it references (operands are atomic).  Collect
             * each -- the same enumeration has_capture_rec walks -- so a lifted
             * body containing a delegated op that captures a Copy value (scalar or
             * owning-free by-value aggregate, e.g. `(.field p)`) is admitted with
             * the operand riding the env.  Composites this scan does not enumerate
             * bail to fallback (cs->ok = false), matching has_capture_rec's
             * conservative default. */
            /* A delegated (direct-emitted) op.  Whatever its shape -- an rc op, a
             * field read, a struct build, a direct/indirect call, or a delegated
             * composite (match / while / for / do / let / ...) -- the complete set
             * of enclosing names it references is exactly its free-variable set.
             * Use the elaborator's free-var walker (the same complete analysis the
             * closure-capture pass uses) so a complex operand like `(+ x 5)` inside
             * a delegated call captures `x`, not just top-level direct-var args.
             * Each free var is filtered against the CPS-bound set / exclude /
             * globals and handed to cap_add, which admits Copy captures (scalar or
             * owning-free by-value aggregate) and bails to fallback on a non-Copy
             * capture.  If the walker ever missed a free var, that var would be an
             * undeclared C name in the lifted helper -- a compile error, not a
             * silent miscompile. */
            const Expr *le = t->as.letraw.e;
            /* U2: a delegated (call/cc f)/(escape f) references its enclosing
             * captures through the receiver `f`; collect_free_vars now descends
             * into an EX_CALLCC receiver (elab_core.c), so a capturing receiver's
             * enclosing locals are surfaced here and ride the lifted continuation
             * env (a scalar capture is admitted by cap_add; a non-Copy capture
             * bails to fallback). */
            uint32_t n_fv = 0;
            Binding **fv = collect_free_vars(le, NULL, 0, NULL, 0, &n_fv);
            for (uint32_t i = 0; i < n_fv && cs->ok; i++) {
                const Binding *b = fv[i];
                if (!b || b->is_global || binding_excluded(b)) continue;
                uint32_t _id = b->id; bool _f = (_id != exclude);
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                if (_f) cap_add(cs, b, b->type.kind, &b->type);
            }
            free(fv);
            /* B7/B7b: a `set!` TARGET is a WRITE, not a free-var read, so
             * collect_free_vars never surfaces it -- but the lifted body writes the
             * shared cell `*m`, so `m` (the cell pointer) must be captured.  Keyed
             * on the promotion itself (is_byref_mut), which covers both the
             * continuation store (B7) and any other write a lifted body makes to
             * an enclosing mutable (B7b).  A NON-promoted target is a mutable
             * local to this body -- no capture, it is an ordinary local. */
            const Binding *bref = set_mut_target(le);
            if (bref && !is_byref_mut(bref)) bref = NULL;
            if (bref && !bref->is_global && !binding_excluded(bref)) {
                uint32_t _id = bref->id; bool _f = (_id != exclude);
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                if (_f) cap_add(cs, bref, bref->type.kind, &bref->type);
            }
            bound[nb] = t->as.letraw.x.id;
            collect_caps_rec(t->as.letraw.body, exclude, bound, nb + 1, cs); return;
        }
        case CT_RESET:
            /* A nested reset sitting in a lifted continuation (two sibling nested
             * resets: the second lands in the first's continuation, e.g.
             * `(reset (+ (reset (shift ...)) (reset (shift ...))))`).  Its
             * delimited body runs under its own (separately lifted) prompt and its
             * continuation continues in this same helper, so collect the free
             * captures of both -- binding the reset value for the continuation --
             * rather than bailing.  emit_reset recurses to emit the nested prompt;
             * this just enumerates the captures its lifted helpers ride. */
            collect_caps_rec(t->as.reset.delim, exclude, bound, nb, cs);
            bound[nb] = t->as.reset.x.id;
            collect_caps_rec(t->as.reset.body, exclude, bound, nb + 1, cs); return;
        case CT_SHIFT:
            /* The delimited body under a nested reset ends in an (abortive) shift;
             * its body's captures ride the shift-body helper's env.  Walk it so a
             * capture threaded through the enclosing continuation is not missed. */
            collect_caps_rec(t->as.shift.body, exclude, bound, nb, cs); return;
        case CT_AWAIT:
            /* F3 gap-2: a nested `await` in a lifted (RESET_CONT) await continuation
             * -- e.g. the second of two sequential awaits.  Its future atom may be a
             * capture the lifted frame must carry; the awaited value binds x for the
             * rest of the continuation. */
            COL_ATOM(&t->as.await.fut);
            bound[nb] = t->as.await.x.id;
            collect_caps_rec(t->as.await.body, exclude, bound, nb + 1, cs); return;
        case CT_PERFORM:
            /* Track A: a nested `perform` in a lifted (RESUME_CONT) perform
             * continuation -- the second of two sequential performs.  Its arg atoms
             * may be captures the lifted frame must carry; the performed-effect
             * result binds x for the rest of the continuation. */
            for (uint32_t i = 0; i < t->as.perform.n; i++) COL_ATOM(&t->as.perform.args[i]);
            bound[nb] = t->as.perform.x.id;
            collect_caps_rec(t->as.perform.body, exclude, bound, nb + 1, cs); return;
        case CT_HANDLE:
            /* A nested `handle` in a lifted continuation (two SEQUENTIAL handles:
             * the second lands in the first's continuation -- effect-abort `main`).
             * Its delim runs under a separately-lifted prompt, and each handler case
             * is lifted into its own frame; those frames' envs are BUILT in this
             * enclosing helper's scope, so their free captures must ride this helper
             * too (mirrors CT_RESET / CT_AWAIT / CT_PERFORM above).  Each case binds
             * its effect params + `k`; the continuation (handle.body) binds the
             * handle result and continues in this same helper. */
            collect_caps_rec(t->as.handle.delim, exclude, bound, nb, cs);
            for (uint32_t ci = 0; ci < t->as.handle.n_cases && cs->ok; ci++) {
                const CHandleCase *hc = &t->as.handle.cases[ci];
                uint32_t cnb = nb;
                for (uint32_t pi = 0; pi < hc->n_params && cnb < CC_MAX_BOUND; pi++)
                    if (hc->params[pi]) bound[cnb++] = hc->params[pi]->id;
                if (hc->k && cnb < CC_MAX_BOUND) bound[cnb++] = hc->k->id;
                collect_caps_rec(hc->case_body, exclude, bound, cnb, cs);
            }
            if (nb < CC_MAX_BOUND) bound[nb] = t->as.handle.x.id;
            collect_caps_rec(t->as.handle.body, exclude, bound, nb + 1, cs); return;
        case CT_CALLCC: {
            /* The receiver f may capture enclosing locals (build_callcc admits a
             * capturing closure).  Surface its free vars -- collect_free_vars
             * descends into the EX_CALLCC receiver -- and ride each scalar capture
             * in the lifted env (a non-Copy capture bails to fallback via cap_add),
             * mirroring the CT_LETRAW-delegated callcc.  Then bind x and walk body. */
            uint32_t n_fv = 0;
            Binding **fv = collect_free_vars(t->as.callcc.e, NULL, 0, NULL, 0, &n_fv);
            for (uint32_t i = 0; i < n_fv && cs->ok; i++) {
                const Binding *cb = fv[i];
                if (!cb || cb->is_global || binding_excluded(cb)) continue;
                uint32_t _id = cb->id; bool _f = (_id != exclude);
                for (int _i = 0; _i < nb; _i++) if (bound[_i] == _id) { _f = false; break; }
                if (_f) cap_add(cs, cb, cb->type.kind, &cb->type);
            }
            free(fv);
            bound[nb] = t->as.callcc.x.id;
            collect_caps_rec(t->as.callcc.body, exclude, bound, nb + 1, cs); return;
        }
        default:
            /* Nested control ops in a lifted body: out of this collector's scope. */
            cs->ok = false; return;
    }
    #undef COL_ATOM
    #undef COL_VAREXPR
}

/* Collect the scalar source captures of `body` (excluding the value param
 * `exclude`).  Returns true and fills `cs` when every capture is collectable;
 * false means the zero-capture fallback must be kept. */
/* cps-while-native (cell-carried vars): the enclosing loop helper's
 * loop-INVARIANT extra params (emit_loop's `inv`) while its body emits.  A
 * lifted frame inside the body that carries the back-edge (CT_CONTINUE)
 * re-enters the helper with those names appended (emit_continue), so they
 * must ride the frame's env even though the frame's own term never mentions
 * them.  NULL outside a loop body. */
static const CapSet *g_loop_inv;

static bool term_has_continue(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_CONTINUE: return true;
        case CT_LETVAL:  return term_has_continue(t->as.letval.body);
        case CT_LETPRIM: return term_has_continue(t->as.letprim.body);
        case CT_LETCALL: return term_has_continue(t->as.letcall.body);
        case CT_LETRAW:  return term_has_continue(t->as.letraw.body);
        case CT_LETCONT: return term_has_continue(t->as.letcont.jbody)
                             || term_has_continue(t->as.letcont.body);
        case CT_IF:      return term_has_continue(t->as.if_.then_)
                             || term_has_continue(t->as.if_.else_);
        case CT_MATCH:   for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                             if (term_has_continue(t->as.match.arms[i].body)) return true;
                         return false;
        case CT_RESET:   return term_has_continue(t->as.reset.delim)
                             || term_has_continue(t->as.reset.body);
        case CT_SHIFT:   return term_has_continue(t->as.shift.body);
        case CT_HANDLE:
            if (term_has_continue(t->as.handle.delim) || term_has_continue(t->as.handle.body))
                return true;
            for (uint32_t i = 0; i < t->as.handle.n_cases; i++)
                if (term_has_continue(t->as.handle.cases[i].case_body)) return true;
            return false;
        case CT_PERFORM: return term_has_continue(t->as.perform.body);
        case CT_AWAIT:   return term_has_continue(t->as.await.body);
        case CT_RESUME:  return term_has_continue(t->as.resume.body);
        case CT_CLONEABLE: return term_has_continue(t->as.cloneable.body);
        case CT_CALLCC:  return term_has_continue(t->as.callcc.body);
        default: return false;
    }
}

static bool collect_caps(const CTerm *body, uint32_t exclude, CapSet *cs) {
    uint32_t bound[CC_MAX_BOUND];
    cs->n = 0; cs->ok = true;
    g_cap_single_shot = true;   /* a reset/handle/perform continuation or shift body */
    collect_caps_rec(body, exclude, bound, 0, cs);
    g_cap_single_shot = false;
    if (cs->ok && g_loop_inv && g_loop_inv->n > 0 && term_has_continue(body)) {
        for (int i = 0; i < g_loop_inv->n && cs->ok; i++) {
            bool dup = false;
            for (int k = 0; k < cs->n; k++) {
                if (g_loop_inv->b[i] ? (cs->b[k] == g_loop_inv->b[i])
                                     : (!cs->b[k] && cs->cvid[k] == g_loop_inv->cvid[i]))
                    { dup = true; break; }
            }
            if (dup) continue;
            if (cs->n >= CC_MAX_CAPS) { cs->ok = false; break; }
            cs->b[cs->n] = g_loop_inv->b[i];
            cs->cvname[cs->n] = g_loop_inv->cvname[i];
            cs->cvid[cs->n] = g_loop_inv->cvid[i];
            cs->ty[cs->n] = g_loop_inv->ty[i];
            cs->type[cs->n] = g_loop_inv->type[i];
            cs->polyfn[cs->n] = g_loop_inv->polyfn[i];
            cs->owning[cs->n] = false;
            cs->n++;
        }
    }
    return cs->ok;
}

/* ---- E3-lite: leak-clean owning captures (borrow-only) ------------------ *
 * An owning value captured into a MULTI-SHOT continuation env is only ever
 * *reachable* here in a BORROW-ONLY shape: a case that consumes (drops/moves)
 * the capture without the enclosing fn also consuming it evicts upstream (the
 * fn's scope-exit auto-drop becomes an unlowered EX_DEFER -- see
 * docs/archive/cps-handler-case-consumes-owning-capture-evicts.md).  For a
 * borrow-only capture the owner (the enclosing fn) drops the value exactly once
 * on its straight-line path, so the env needs neither clone nor drop: it rides
 * by a bare shallow alias -- leak-clean, no double-free, and the observed value
 * is exact (no incref inflation).  Only a *provably consuming* rc capture (the
 * rare "drop in the case AND straight-line in the fn" double-consume that still
 * reaches CPS) keeps the incref-on-read-out (Option A) path, which is memory-safe
 * (balanced) though it retains the env's clone.  This is the reachable-shape
 * realization of the env-capture plan's leak-clean bar without a DK teardown. */

/* A delegated op's Expr `e` is a pure borrow READ of `bid` -- rc/strong-count,
 * rc->ptr, or (weak ..), whose direct operand is `bid`.  These read the handle
 * without releasing or moving it (the result is a scalar / a non-owning borrow),
 * so the capture stays owned by the caller. */
static bool expr_is_pure_borrow_of(const Expr *e, uint32_t bid) {
    while (e && e->kind == EX_ASCRIBE) e = e->as.ascribe_.inner;
    if (!e) return false;
    const Expr *inner = NULL;
    switch (e->kind) {
        /* rc/weak ops READ their handle operand without releasing or moving it
         * (the result is a scalar count, a borrowed inner ptr, or a fresh weak),
         * so they are a pure borrow of the operand.  The operand may itself be an
         * OWNING field read `(.r o)` of the captured by-value aggregate: the op
         * then borrows that owning field, leaving `o` (and its owning fields)
         * intact, so it is a pure borrow of the root `o`.  The shared peel below
         * walks the operand's field-read chain to that root -- letting a borrow
         * THROUGH an owning field read be recognized, not only a bare-var
         * operand `(rc/strong-count o)`. */
        case EX_RC_COUNT: inner = e->as.rc_count_.expr; break;
        case EX_RC_PTR:   inner = e->as.rc_ptr_.expr;   break;
        case EX_WEAK:     inner = e->as.weak_.expr;     break;
        case EX_GET_FIELD:
            /* Reading a NON-owning field `(.f o)` of the captured by-value
             * aggregate is a pure borrow of `o` -- it copies a scalar out, leaving
             * `o` (and its owning fields) intact.  An OWNING field read yields an
             * alias that could be consumed, so a STANDALONE owning field read is
             * not provably borrow-only: conservatively reject (the capture then
             * evicts / rides E3).  (An owning field read WRAPPED in an rc/weak op
             * is handled by those arms above, which borrow rather than move it.) */
            if (e->type.kind == TY_RC || e->type.kind == TY_REF
                || e->type.kind == TY_WEAK || e->type.kind == TY_LREF)
                return false;
            inner = e->as.get_field_.struct_expr;
            break;
        default: return false;
    }
    /* Peel a chain of field reads to the root aggregate (shared by the rc/weak
     * and get-field arms). */
    while (inner) {
        while (inner && inner->kind == EX_ASCRIBE) inner = inner->as.ascribe_.inner;
        if (inner && inner->kind == EX_GET_FIELD) {
            inner = inner->as.get_field_.struct_expr; continue;
        }
        break;
    }
    return inner && inner->kind == EX_VAR && inner->as.var.binding
        && inner->as.var.binding->id == bid;
}

static bool expr_refs_bid(const Expr *e, uint32_t bid) {
    uint32_t nfv = 0;
    Binding **fv = collect_free_vars(e, NULL, 0, NULL, 0, &nfv);
    bool r = false;
    for (uint32_t i = 0; i < nfv; i++) if (fv[i] && fv[i]->id == bid) { r = true; break; }
    free(fv);
    return r;
}

static bool cap_atom_is_bid(const CAtom *a, uint32_t bid) {
    return a->kind == CA_VAR && a->var && a->var->id == bid;
}

/* True iff every reference to owning capture `bid` in this handler-case body
 * (handle_case_ok shape) is a pure borrow read.  Conservative: any reference
 * that is not a recognized borrow -- a consuming delegated op, a move into a
 * call arg / delivery, or anything unmodelled -- returns false (keep incref). */
static bool owning_cap_borrow_only(const CTerm *t, uint32_t bid) {
    while (t) {
        switch (t->kind) {
            case CT_APPCONT:
                return !cap_atom_is_bid(&t->as.appcont.v, bid);
            case CT_LETVAL:
                if (cap_atom_is_bid(&t->as.letval.v, bid)) return false;
                t = t->as.letval.body; break;
            case CT_LETPRIM:
                for (uint32_t i = 0; i < t->as.letprim.n; i++)
                    if (cap_atom_is_bid(&t->as.letprim.args[i], bid)) return false;
                t = t->as.letprim.body; break;
            case CT_LETCALL:
                for (uint32_t i = 0; i < t->as.letcall.n; i++)
                    if (cap_atom_is_bid(&t->as.letcall.args[i], bid)) return false;
                t = t->as.letcall.body; break;
            case CT_LETRAW:
                if (expr_refs_bid(t->as.letraw.e, bid)
                    && !expr_is_pure_borrow_of(t->as.letraw.e, bid))
                    return false;
                t = t->as.letraw.body; break;
            case CT_RESUME:
                if (cap_atom_is_bid(&t->as.resume.v, bid) || cap_atom_is_bid(&t->as.resume.k, bid))
                    return false;
                t = t->as.resume.body; break;
            case CT_IF:
                if (cap_atom_is_bid(&t->as.if_.cond, bid)) return false;
                return owning_cap_borrow_only(t->as.if_.then_, bid)
                    && owning_cap_borrow_only(t->as.if_.else_, bid);
            default: return false;
        }
    }
    return true;
}

/* Collect a handler case body's scalar captures, excluding the case's own params
 * and continuation k (bound by the DKHandler signature).  Same collect-or-bail
 * contract as collect_caps. */
static bool collect_caps_case(const CTerm *body, const CHandleCase *hc, CapSet *cs) {
    g_excl_n = 0;
    for (uint32_t i = 0; i < hc->n_params && g_excl_n < 8; i++)
        g_excl_b[g_excl_n++] = hc->params[i];
    if (hc->k && g_excl_n < 8) g_excl_b[g_excl_n++] = hc->k;
    uint32_t bound[CC_MAX_BOUND];
    cs->n = 0; cs->ok = true;
    g_cap_single_shot = false;   /* a handler case body runs once per perform, not single-shot */
    collect_caps_rec(body, UINT32_MAX, bound, 0, cs);
    g_excl_n = 0;
    /* E-borrow: a borrow-only owning capture rides by a bare shallow alias
     * (owning=false) -- leak-clean, since the enclosing fn owns and drops it
     * exactly once.  A provably-consuming capture keeps owning=true, but only an
     * rc handle has a scalar incref (rc_strong_increment) for the read-out clone;
     * a consuming aggregate / carrier handle has no such glue (it needs E3's env
     * teardown), so evict it rather than emit a wrong incref. */
    if (cs->ok)
        for (int i = 0; i < cs->n; i++) {
            if (!cs->owning[i] || !cs->b[i]) continue;
            if (owning_cap_borrow_only(body, cs->b[i]->id))
                cs->owning[i] = false;
            else if (cs->ty[i] != TY_RC)
                cs->ok = false;
        }
    return cs->ok;
}

/* A reset continuation must be self-contained: every KK_VAR it delivers to must
 * name a join defined within it (its exits otherwise are KK_RET, which the
 * lifted helper maps to its own k).  Rejects a body that escapes to an
 * enclosing join. */
static bool joins_closed_rec(const CTerm *t, uint32_t *def, int nd) {
    if (!t || nd >= CC_MAX_BOUND) return t == NULL;
    switch (t->kind) {
        case CT_APPCONT:
            if (t->as.appcont.kont.kind == KK_VAR) {
                for (int i = 0; i < nd; i++) if (def[i] == t->as.appcont.kont.id) return true;
                return false;
            }
            return true;
        case CT_LETVAL:  return joins_closed_rec(t->as.letval.body, def, nd);
        case CT_LETPRIM: return joins_closed_rec(t->as.letprim.body, def, nd);
        case CT_LETCALL: return joins_closed_rec(t->as.letcall.body, def, nd);
        case CT_LETRAW:  return joins_closed_rec(t->as.letraw.body, def, nd);
        case CT_TAILCALL:
            if (t->as.tailcall.kont.kind == KK_VAR) {
                for (int i = 0; i < nd; i++) if (def[i] == t->as.tailcall.kont.id) return true;
                return false;
            }
            return true;
        case CT_IF:
            return joins_closed_rec(t->as.if_.then_, def, nd)
                && joins_closed_rec(t->as.if_.else_, def, nd);
        case CT_LETCONT:
            def[nd] = t->as.letcont.j.id;
            return joins_closed_rec(t->as.letcont.jbody, def, nd + 1)
                && joins_closed_rec(t->as.letcont.body, def, nd + 1);
        case CT_RESUME: return joins_closed_rec(t->as.resume.body, def, nd);
        case CT_CLONEABLE: return joins_closed_rec(t->as.cloneable.body, def, nd);
        case CT_CALLCC: return joins_closed_rec(t->as.callcc.body, def, nd);
        case CT_PERFORM:
            /* Track A (A2): a nested `perform` in a reset/handle continuation
             * body.  The perform delivers to a handler (not a join); its joins are
             * in its own continuation, so recurse there.  term_core_ok already
             * gates whether the nested perform is emittable (A1). */
            return joins_closed_rec(t->as.perform.body, def, nd);
        case CT_HANDLE:
            /* A nested `handle` in a reset/handle CONTINUATION (e.g. two sequential
             * `handle`s: the first's continuation contains the second -- effect-abort
             * `main`).  The continuation (handle.body) runs in the SAME join scope, so
             * recurse with the current def set.  The delim and handler cases are each
             * lifted into their OWN DK frame (emit_lifted, n_joins reset), so a join
             * they reference must be defined WITHIN them -- an outer-join reference
             * there is unemittable; check them with a FRESH (empty) def set so such a
             * reference is correctly rejected.  term_core_ok already gates the nested
             * handle's structural emittability (handle_delim_ok / handle_case_ok). */
            {
                uint32_t hdef[CC_MAX_BOUND];
                if (!joins_closed_rec(t->as.handle.delim, hdef, 0)) return false;
                for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++)
                    if (!joins_closed_rec(t->as.handle.cases[ci].case_body, hdef, 0))
                        return false;
                return joins_closed_rec(t->as.handle.body, def, nd);
            }
        /* cps-while-native: a back-edge references the loop helper, not a join; a
         * CT_LOOP's internal joins are a separate scope (checked when its own body
         * is admitted).  Neither leaves an outer join open here. */
        case CT_CONTINUE: return true;
        case CT_LOOP:
            if (t->as.loop.result_kont.kind == KK_VAR) {
                for (int i = 0; i < nd; i++) if (def[i] == t->as.loop.result_kont.id) return true;
                return false;
            }
            return true;
        default: return false;
    }
}

static bool reset_body_ok(const CTerm *t) {
    if (!term_core_ok(t)) return false;
    uint32_t def[CC_MAX_BOUND];
    return joins_closed_rec(t, def, 0);
}

/* A shift body in the C3 subset: straight-line (letval/letprim/letcall) ending
 * in delivery of a scalar atom to the prompt.  No branches, tail calls, joins,
 * or nested delimiters. */
/* An abortive shift receiver `(fn [k : cont] ...)` that ignores its continuation
 * is applied to the shift's value operand, binding its `cont`-typed parameter to
 * a placeholder literal (`let k = 0`, typed TY_CONT).  The binding is dead -- the
 * receiver never references `k` -- so the placeholder never crosses a slot; it is
 * only there to keep the receiver's parameter in scope.  The generic atom_ok slot
 * gate rejects a TY_CONT literal (a continuation is not slot-representable), which
 * would evict an otherwise-native abortive shift.  Admit the dead cont placeholder
 * here (scoped to the shift body), where the DK backend materializes it as an
 * unused local. */
static bool shift_cont_placeholder(const CAtom *a) {
    return (a->kind == CA_INT || a->kind == CA_UNIT || a->kind == CA_BOOL)
        && a->ty == TY_CONT;
}

static bool shift_body_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            return t->as.appcont.kont.kind == KK_PROMPT && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return (atom_ok(&t->as.letval.v) || shift_cont_placeholder(&t->as.letval.v))
                && shift_body_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec) || is_println_shape(t->as.letprim.spec->shape))
                return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return shift_body_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return shift_body_ok(t->as.letcall.body);
        case CT_LETRAW:
            return letraw_ok(t) && shift_body_ok(t->as.letraw.body);
        case CT_CALLCC:
            /* A call/cc / escape hoisted into the shift body's straight-line
             * abort-value computation (e.g. `(shift (fn [v] v) (+ 1 (escape f)))`):
             * emit_lifted emits the body through emit_term, which lowers CT_CALLCC
             * via the native setjmp landing (emit_callcc), so it stays in the CT-IR
             * path rather than delegating to emit_cps.c. */
            return (slot_ok_t(t->as.callcc.x.type, t->as.callcc.x.ty) || t->as.callcc.x.ty == TY_NIL)
                && shift_body_ok(t->as.callcc.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && shift_body_ok(t->as.if_.then_) && shift_body_ok(t->as.if_.else_);
        default: return false;
    }
}

/* A perform continuation (C4): straight-line, delivered to KK_RET as a scalar
 * (the lifted DKFrame returns it).  No branches / tail calls / joins / nested
 * control. */
static bool perform_body_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            /* Delivered to the function's return continuation (KK_RET) or, when
             * the perform is the tail of a delimited body, to the enclosing
             * prompt (KK_PROMPT); the lifted frame returns the value either way. */
            return (t->as.appcont.kont.kind == KK_RET || t->as.appcont.kont.kind == KK_PROMPT)
                && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return atom_ok(&t->as.letval.v) && perform_body_ok(t->as.letval.body);
        case CT_LETPRIM:
            /* println/print IS emittable in a perform continuation: emit_lifted
             * emits the frame body through emit_term (the same path a handler case
             * uses, handle_case_ok, which admits println), so the print statement
             * lands in the LH_PERFORM_CONT frame.  A multi-shot resume re-runs the
             * frame and prints again -- the correct semantics (the continuation
             * genuinely runs more than once), exactly as a re-run handler case does.
             * Unconditional since cps-tramp-resume graduated (2026-07-19); the
             * historical exclusion it used to preserve was conservative from
             * before the case-body println path, and is gone. */
            if (!shape_supported(t->as.letprim.spec))
                return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return perform_body_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return perform_body_ok(t->as.letcall.body);
        case CT_LETRAW:
            return letraw_ok(t) && perform_body_ok(t->as.letraw.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && perform_body_ok(t->as.if_.then_) && perform_body_ok(t->as.if_.else_);
        default: return false;
    }
}

/* Track A (multi-suspension continuations): a perform continuation that is a
 * FULL CPS body containing a NESTED control op (a further `perform`) -- the
 * two-perform shape `(let [a (perform E)] (let [b (perform E)] (+ a b)))`.  It is
 * lifted as a RESUME-FRAME (LH_RESUME_CONT): the frame receives its run-time
 * downstream chain `__kont` (the reinstalled-handler tail dk_perform splices in)
 * and threads it, so the nested perform finds the correct enclosing handler and
 * the value is delivered exactly once.  This mirrors the F3 await gap-2
 * (await_cont_reset_ok / emit_await), but a nested PERFORM re-dispatches to a
 * handler (unlike await, which shifts to the root prompt), which is exactly why
 * the frame must carry its run-time rest rather than a bare `next = cur_k`.
 *
 * Bounded like await gap-2: a cps->cps TAIL CALL is rejected (it would recurse
 * unboundedly through dk_invoke -- an O(N) resume stack), and so is any other
 * nested control op (handle/shift/await/callcc mixed in) -- those are A2/A3. */
static bool perform_cont_reset_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            /* E7/C1 (cps-tramp-resume): a jump to a join (KK_VAR) bound by an
             * enclosing CT_LETCONT in this same continuation -- admitted so a branch
             * whose merged result feeds a subsequent perform lifts into the frame. */
            if (t->as.appcont.kont.kind == KK_VAR)
                return atom_ok(&t->as.appcont.v);
            return (t->as.appcont.kont.kind == KK_RET || t->as.appcont.kont.kind == KK_PROMPT)
                && atom_ok(&t->as.appcont.v);
        case CT_LETCONT:
            /* E7/C1: a JOIN in the perform continuation -- `letcont j(x) = jbody in
             * body`, where body branches and jumps to j, and jbody (the merged
             * continuation) may itself perform.  Admit when both sides are reset-ok;
             * emit_lifted lowers the join as local control flow inside the frame. */
            return perform_cont_reset_ok(t->as.letcont.jbody)
                && perform_cont_reset_ok(t->as.letcont.body);
        case CT_LETVAL:
            return atom_ok(&t->as.letval.v) && perform_cont_reset_ok(t->as.letval.body);
        case CT_LETPRIM:
            /* E7/C1: println/print in a Track-A resume-frame continuation -- same
             * emit_term path as a handler case (handle_case_ok), so the print lands
             * in the LH_RESUME_CONT frame body.  Unconditional since
             * cps-tramp-resume graduated (2026-07-19); the historical
             * flag-off exclusion this mirrored is gone. */
            if (!shape_supported(t->as.letprim.spec))
                return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return perform_cont_reset_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!call_arg_ok(&t->as.letcall.args[i], true)) return false;
            return perform_cont_reset_ok(t->as.letcall.body);
        case CT_LETRAW:
            return letraw_ok(t) && perform_cont_reset_ok(t->as.letraw.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && perform_cont_reset_ok(t->as.if_.then_)
                && perform_cont_reset_ok(t->as.if_.else_);
        case CT_PERFORM:
            /* The nested perform's args must be slot atoms; its OWN continuation
             * is emitted straight-line (perform_body_ok -> a value-transform frame
             * threading __kont) or as a further resume-frame (this predicate). */
            for (uint32_t i = 0; i < t->as.perform.n; i++)
                if (!atom_ok(&t->as.perform.args[i])) return false;
            return perform_body_ok(t->as.perform.body)
                || perform_cont_reset_ok(t->as.perform.body);
        case CT_TAILCALL:
            /* E7 (cps-tramp-resume): admit a TAIL CALL in the perform continuation.
             * It lifts as a RESUME_FRAME whose rfn threads its run-time `rest` (the
             * reinstalled-handler tail) as the callee's continuation, and the
             * trampolined tail-resume runtime keeps the recursion flat instead of
             * the O(N) dk_invoke stack this predicate otherwise (correctly) rejects.
             * Unconditional since cps-tramp-resume graduated.  Args must be
             * slot atoms. */
            /* perform-continuation-tail-call-arg-gate-assumes-direct: the gate
             * used to pass cps_to_direct=true unconditionally, so a by-value
             * aggregate atom (`(some 1)`) was refused even when the callee is
             * COLORED and threads `__cps` params of the matching ABI -- the
             * CT_TAILCALL rule in term_core_ok already asks the callee.  A
             * `(perform (Tick))` followed by `(option-eq? (some 1) (some 1) f)`
             * evicted BODY-STRUCT-CORE; the same call BEFORE the perform lowered. */
            {
                bool tc_direct = !binding_cps_reachable(t->as.tailcall.fn);
                for (uint32_t i = 0; i < t->as.tailcall.n; i++)
                    if (!call_arg_ok(&t->as.tailcall.args[i], tc_direct)) return false;
            }
            return true;
        case CT_CONTINUE:
            /* cps-while-native: a loop back-edge in a perform continuation -- the
             * ESCAPING-effect shape (`while` body performs an effect handled by an
             * OUTER handler; the perform's own continuation carries the back-edge).
             * Like CT_TAILCALL it re-enters the loop helper threading __kont, so it
             * must ride an LH_RESUME_CONT resume-frame (which has __kont) -- guaranteed
             * because perform_body_ok has no CT_CONTINUE case, so emit_perform takes
             * the resume-frame branch.  Args must be slot atoms. */
            for (uint32_t i = 0; i < t->as.cont_.n; i++)
                if (!call_arg_ok(&t->as.cont_.args[i], true)) return false;
            return true;
        default: return false;   /* any other nested control op: evict */
    }
}

/* F3 gap-2: an await continuation that is a FULL CPS body (a branch, or a further
 * `await`) but carries a statically-BOUNDED number of await suspensions -- so it
 * is safe to lift as a RESUME-FRAME (LH_RESUME_CONT: the frame receives its
 * downstream chain at run time; its `next` is the real, borrowed enclosing
 * chain).  The one thing that is NOT bounded
 * is a TAIL CALL: a cps->cps tail call threads __kont and can recurse, and a
 * ready-future inline resume then recurses through dk_invoke in O(N) C stack
 * (worse than the direct TCO path -- a recursive await is by design left to the
 * direct emitter; see docs/archive/cps-async-recursive-await-eviction.md).  ALL
 * tail calls are rejected here,
 * not just cps->cps ones: whether a callee is CPS-emitted (binding_in_s) is not
 * yet settled while this predicate runs during S-classification (a self-recursive
 * callee reads back as `false` mid-fixpoint), so keying the reject on that flag is
 * unsound -- it would admit the very recursion it means to exclude.  A tail call
 * is never part of the gap-2 shape anyway (two sequential awaits end in an
 * appcont), so rejecting all of them keeps the whitelist sound and still admits
 * gap 2.  Other nested control ops (reset/handle/perform/shift/resume/callcc mixed
 * into an await continuation) also stay out of scope and evict. */
static bool await_cont_reset_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            return (t->as.appcont.kont.kind == KK_RET || t->as.appcont.kont.kind == KK_PROMPT)
                && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return atom_ok(&t->as.letval.v) && await_cont_reset_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec)) return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return await_cont_reset_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!call_arg_ok(&t->as.letcall.args[i], true)) return false;
            return await_cont_reset_ok(t->as.letcall.body);
        case CT_LETRAW:
            return letraw_ok(t) && await_cont_reset_ok(t->as.letraw.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && await_cont_reset_ok(t->as.if_.then_)
                && await_cont_reset_ok(t->as.if_.else_);
        case CT_AWAIT:
            return atom_ok(&t->as.await.fut) && await_cont_reset_ok(t->as.await.body);
        default: return false;   /* CT_TAILCALL and any nested control op: evict */
    }
}

/* A handler case body (C4): straight-line with `resume` (dk_invoke), delivered
 * to the prompt (KK_PROMPT) as a scalar.  resume.k is the continuation binding
 * (not scalar-checked); resume.v and other atoms are scalar. */
static bool handle_case_ok_rec(const CTerm *t);

/* The body of a CT_LOOP that sits inside a handler CASE.  The loop is emitted
 * as its own synthesized helper (emit_loop), so its body does not follow the
 * case-body grammar: its exit arm delivers to the helper's KK_RET (which the
 * case context turns into a plain `return`), its back-edge is CT_CONTINUE, and
 * a `resume` inside it is an ordinary dk_invoke against the case's `k`
 * threaded in as a loop-invariant param.
 *
 * Deliberately NARROWER than term_core_ok: no CT_PERFORM / CT_HANDLE /
 * CT_RESET / CT_AWAIT / nested CT_LOOP.  In the case context the loop helper
 * is entered with a NULL DK continuation (its result is RETURNED, not
 * dk_run), so any interior op that would thread `__kont` into the DK machine
 * would hand it a null chain at run time.  Admitting those means giving the
 * helper a real chain to thread first; until then they evict, which is the
 * safe answer. */
static bool case_loop_body_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            /* KK_RET: the loop's exit delivery (build_loop constructs exactly
             * one).  KK_VAR: a jump to a join within the loop body. */
            return (t->as.appcont.kont.kind == KK_RET
                    || t->as.appcont.kont.kind == KK_VAR)
                && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return atom_ok(&t->as.letval.v) && case_loop_body_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec))
                return false;
            if (!is_println_shape(t->as.letprim.spec->shape))
                for (uint32_t i = 0; i < t->as.letprim.n; i++)
                    if (!atom_ok(&t->as.letprim.args[i])) return false;
            return case_loop_body_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return case_loop_body_ok(t->as.letcall.body);
        case CT_LETCONT:
            return (slot_ok_t(t->as.letcont.param.type, t->as.letcont.param.ty)
                    || t->as.letcont.param.ty == TY_NIL)
                && case_loop_body_ok(t->as.letcont.jbody)
                && case_loop_body_ok(t->as.letcont.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && case_loop_body_ok(t->as.if_.then_)
                && case_loop_body_ok(t->as.if_.else_);
        case CT_RESUME:
            /* The multi-shot fold: `(resume k i)` each iteration.  dk_invoke
             * re-invokes the same k exactly as the straight-line double-resume
             * a case already supports; the loop only changes how many times. */
            return t->as.resume.k.kind == CA_VAR && atom_ok(&t->as.resume.v)
                && case_loop_body_ok(t->as.resume.body);
        case CT_CONTINUE:
            for (uint32_t i = 0; i < t->as.cont_.n; i++)
                if (!atom_ok(&t->as.cont_.args[i])) return false;
            return true;
        default:
            return false;
    }
}

/* Admission entry for a handler CASE body.
 *
 * emit_lifted gives each case its own DK frame with a fresh join stack, so
 * every KK_VAR jump the case makes must name a join the case itself binds --
 * a jump that escapes to an enclosing join has no slot in the frame.  That is
 * exactly the check joins_closed_rec performs with a fresh def set, and it is
 * already what the nested-handle arm of joins_closed_rec applies to a case
 * body one level down; this applies it at the top level too, which is what
 * makes admitting CT_LETCONT / KK_VAR below safe rather than hopeful. */
static bool handle_case_ok(const CTerm *t) {
    if (!handle_case_ok_rec(t)) return false;
    uint32_t def[CC_MAX_BOUND];
    return joins_closed_rec(t, def, 0);
}

static bool handle_case_ok_rec(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            /* A jump to a join the case binds (KK_VAR), or the case's own
             * delivery of its value to the handle's prompt.  The KK_VAR arm is
             * what lets a statement-position conditional -- which lowers to
             * `letcont j(x) = <rest> in if c then (j a) else (j b)` -- land in
             * a case body; handle_case_ok's joins_closed_rec has already
             * established that j is bound within this case. */
            if (t->as.appcont.kont.kind == KK_VAR)
                return atom_ok(&t->as.appcont.v);
            return t->as.appcont.kont.kind == KK_PROMPT && atom_ok(&t->as.appcont.v);
        case CT_LETCONT:
            /* A join point in the case body.  emit_term lowers CT_LETCONT as
             * local control flow (a slot plus a label) inside whatever frame it
             * is emitting, and emit_lifted emits a case through emit_term, so
             * the join lands in the case's own frame.  Without this arm a
             * conditional in NON-TAIL position inside a handler clause evicted
             * the enclosing function from the CPS backend, and its `perform`
             * then reached the direct emitter, which has no lowering for one.
             * (A `while` in a clause is a different blocker -- CT_LOOP, still
             * unadmitted here.)  See
             * docs/archive/handler-clause-statement-if-ices-emitter.md. */
            return (slot_ok_t(t->as.letcont.param.type, t->as.letcont.param.ty)
                    || t->as.letcont.param.ty == TY_NIL)
                && handle_case_ok_rec(t->as.letcont.jbody)
                && handle_case_ok_rec(t->as.letcont.body);
        case CT_LETVAL:
            return atom_ok(&t->as.letval.v) && handle_case_ok_rec(t->as.letval.body);
        case CT_LETPRIM:
            /* println/print IS emittable in a lifted handler case: emit_term's
             * CT_LETPRIM path emits the print statement (emit_println) and binds
             * the nil placeholder, exactly as at top level.  A handler that prints
             * (`(Write [s] k) (do (println s) (resume k 0))`) is the commonest
             * effect shape.  Admitting it was gated on two now-fixed soundness
             * bugs: the taint-via-fn-value-data-flow split (PI-1) and multi-effect
             * deep-handler re-installation (PI-2). */
            if (!shape_supported(t->as.letprim.spec))
                return false;
            if (!is_println_shape(t->as.letprim.spec->shape))
                for (uint32_t i = 0; i < t->as.letprim.n; i++)
                    if (!atom_ok(&t->as.letprim.args[i])) return false;
            return handle_case_ok_rec(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return handle_case_ok_rec(t->as.letcall.body);
        case CT_RESUME:
            return t->as.resume.k.kind == CA_VAR && atom_ok(&t->as.resume.v)
                && handle_case_ok_rec(t->as.resume.body);
        case CT_LOOP: {
            /* A `while` in the case body (`(Choose [lo hi] ^multishot k)
             * (let [^mut a 0 ^mut i lo] (while (<= i hi) ...) a)` -- the
             * multi-shot fold).  The loop is emitted as its own helper whose
             * result is RETURNED into the case (emit_loop's return-direct
             * mode), so admit its params/inits at the slot gate and its body
             * by the dedicated loop-body grammar.  Joins inside the loop body
             * must close within it -- the helper has its own frame. */
            for (uint32_t i = 0; i < t->as.loop.n_params; i++)
                if (!(slot_ok_t(t->as.loop.params[i].type, t->as.loop.params[i].ty)
                      || t->as.loop.params[i].ty == TY_NIL))
                    return false;
            for (uint32_t i = 0; i < t->as.loop.n_params; i++)
                if (!atom_ok(&t->as.loop.inits[i])) return false;
            uint32_t ldef[CC_MAX_BOUND];
            return case_loop_body_ok(t->as.loop.body)
                && joins_closed_rec(t->as.loop.body, ldef, 0);
        }
        case CT_LETRAW:
            return letraw_ok(t) && handle_case_ok_rec(t->as.letraw.body);
        case CT_CALLCC:
            /* A call/cc / escape hoisted into a handler case body (e.g. `(resume k
             * (+ 1 (escape f)))`): emit_lifted emits the case through emit_term,
             * which lowers CT_CALLCC via the native setjmp landing (emit_callcc),
             * so it stays in the CT-IR path rather than delegating to emit_cps.c. */
            return (slot_ok_t(t->as.callcc.x.type, t->as.callcc.x.ty) || t->as.callcc.x.ty == TY_NIL)
                && handle_case_ok_rec(t->as.callcc.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && handle_case_ok_rec(t->as.if_.then_) && handle_case_ok_rec(t->as.if_.else_);
        case CT_PERFORM:
            /* Effect re-opening (docs/archive/cps-handler-case-effect-reopening-
             * needs-emission.md): a handler CASE body that itself performs an
             * effect handled by an ENCLOSING handler.  The interior perform
             * dispatches against the case's enclosing handler markers -- emit_lifted
             * declares `__kont = dk_case_enclosing_real(...)` for a re-opening case and
             * emit_perform threads it as cur_k (both its straight-line LH_PERFORM_CONT
             * and Track A LH_RESUME_CONT paths).  Args must be slot atoms; the
             * continuation stays in the CASE context (it may resume the case's own
             * `k`), so it is admitted by handle_case_ok, not perform_body_ok. */
            for (uint32_t i = 0; i < t->as.perform.n; i++)
                if (!atom_ok(&t->as.perform.args[i])) return false;
            return handle_case_ok_rec(t->as.perform.body);
        default: return false;
    }
}

/* An owning-value op that allocates or increments a refcount, i.e. creates a
 * drop obligation (rc/of, rc/clone).  rc/strong-count / rc->ptr only read. */
static bool owning_alloc_expr(const Expr *e) {
    return e && (e->kind == EX_RC_OF || e->kind == EX_RC_CLONE);
}

/* True if `e` is `(rc/drop v)` where v is the binding with id `bid`. */
static bool is_rc_drop_of(const Expr *e, uint32_t bid) {
    if (!e || e->kind != EX_RC_DROP) return false;
    const Expr *arg = e->as.rc_drop_.expr;
    while (arg && arg->kind == EX_ASCRIBE) arg = arg->as.ascribe_.inner;
    return arg && arg->kind == EX_VAR && arg->as.var.binding
        && arg->as.var.binding->id == bid;
}

/* Is owning binding `bid` dropped on a straight-line path before any control op,
 * branch, delivery, or the end of the term?  This is the soundness condition for
 * emitting an rc alloc/clone on the CPS path: an abortive `shift` discards its
 * continuation, so a drop that is not reached before the next control op would be
 * dropped from the program (leak / miscompile).  Conservative: any branch
 * (CT_IF / CT_LETCONT) or delivery before the drop returns false (fall back). */
static bool owning_dropped_before_control(const CTerm *t, uint32_t bid) {
    while (t) {
        switch (t->kind) {
            case CT_LETRAW:
                if (is_rc_drop_of(t->as.letraw.e, bid)) return true;
                t = t->as.letraw.body; break;
            case CT_LETVAL:  t = t->as.letval.body;  break;
            case CT_LETPRIM: t = t->as.letprim.body; break;
            case CT_LETCALL: t = t->as.letcall.body; break;
            case CT_LETCONT:
                /* Execution proceeds in the body (which may jump to the join);
                 * the join's jbody is reached via an appcont, i.e. after this
                 * point, so a drop must appear in the body before that jump. */
                t = t->as.letcont.body; break;
            case CT_IF:
                /* Dropped before control iff dropped on BOTH branches (whichever
                 * runs, the value is dead before the branch's own control op). */
                return owning_dropped_before_control(t->as.if_.then_, bid)
                    && owning_dropped_before_control(t->as.if_.else_, bid);
            case CT_RESET:
            case CT_HANDLE:
            case CT_PERFORM:
            case CT_AWAIT:
                /* The owning value crosses into a SINGLE-SHOT continuation (the
                 * reset/handle/perform/await continuation runs at most once -- a second
                 * resume is a hard error, TUR-E0201).  It is now captured into
                 * that continuation's env and dropped there exactly once (or moved
                 * out), which is sound -- so the drop no longer has to precede the
                 * control op.  The capture-admission gates enforce this: an owning
                 * capture is admitted for a single-shot continuation (collect_caps)
                 * but bails for a handler CASE body (collect_caps_case, which runs
                 * per-perform), so a value consumed in a multi-run case still falls
                 * back.  A missed capture surfaces as an undeclared C name (compile
                 * error), never a silent leak/double-free. */
                return true;
            case CT_CLONEABLE:
                /* E3a (owning-cloneable-capture): an owning `rc` the fn owns,
                 * captured ^borrow across a cloneable-reset and dropped AFTER it,
                 * gets the same "no longer has to precede the control op" pass.
                 * Soundness: the E3a admission only lets the frame BORROW the rc
                 * (build_marshal_reset + FA_BORROW), so it is never dropped inside
                 * the multi-shot continuation -- only once by its owner on the
                 * straight-line path after the reset, which completes normally
                 * (the cloneable receiver captures the continuation; it does not
                 * abort past the drop).  Forgetting the drop cannot silently leak:
                 * the auto-inserted scope-exit drop is an EX_DEFER, still
                 * unsupported on the CPS path (it evicts), so an owning capture
                 * without an explicit end-of-scope drop does not compile here.
                 * Unconditional since owning-cloneable-capture graduated
                 * (2026-07-20). */
                return true;
            default: return false;   /* shift (abortive) / unexpected: conservative */
        }
    }
    return false;
}

/* True if `e` is the `(drop! v)` free-shape builtin where v is binding `bid` --
 * the ownership discharge the elaborator injects for a `ref<T>` local, hoisted to
 * before the control op by the CPS backend's ref auto-drop (O1-b, cps_ir.c). */
static bool is_ref_drop_of(const Expr *e, uint32_t bid) {
    if (!e || e->kind != EX_BUILTIN) return false;
    if (!e->as.builtin.spec || e->as.builtin.spec->shape != BS_PREFIX_UNARY_FREE
        || e->as.builtin.n != 1)
        return false;
    const Expr *arg = e->as.builtin.args[0];
    while (arg && arg->kind == EX_ASCRIBE) arg = arg->as.ascribe_.inner;
    return arg && arg->kind == EX_VAR && arg->as.var.binding
        && arg->as.var.binding->id == bid;
}

/* O1-b: is the owning `ref<T>` binding `bid` `drop!`-discharged on a straight-line
 * path before ANY control op, branch that reifies a continuation, delivery, or the
 * end of the term?  This is the P1 admission condition for delegating an EX_REF
 * alloc on the CPS path.  Unlike owning_dropped_before_control (which grants an rc
 * handle a "captured into a single-shot continuation, dropped there" pass at
 * reset/handle/perform), a ref gets NO such pass -- the DK teardown that would drop
 * a captured ref is P2/P3 substrate that does not exist yet -- so a ref crossing
 * ANY control op (abortive or delimited) falls back.  A ref whose drop is not found
 * before a barrier (e.g. a moved ref with no in-scope drop) also falls back. */
static bool ref_dropped_before_control(const CTerm *t, uint32_t bid) {
    while (t) {
        switch (t->kind) {
            case CT_LETRAW:
                if (is_ref_drop_of(t->as.letraw.e, bid)) return true;
                t = t->as.letraw.body; break;
            case CT_LETVAL:  t = t->as.letval.body;  break;
            case CT_LETPRIM: t = t->as.letprim.body; break;
            case CT_LETCALL: t = t->as.letcall.body; break;
            case CT_LETCONT: t = t->as.letcont.body; break;
            case CT_IF:
                return ref_dropped_before_control(t->as.if_.then_, bid)
                    && ref_dropped_before_control(t->as.if_.else_, bid);
            case CT_PERFORM:
            case CT_HANDLE:
            case CT_RESET:
            case CT_AWAIT:
                /* EXPERIMENTAL (cps-tramp-resume): grant a ref the same single-shot
                 * continuation pass RC already has (owning_dropped_before_control) --
                 * captured into the single-shot continuation's env, freed there.
                 * Whether the capture+free substrate exists for a ref is verified by
                 * ASan on effect-ref.  Unconditional since cps-tramp-resume
                 * graduated (2026-07-19). */
                return true;
            default: return false;   /* control op / delivery / end: not before */
        }
    }
    return false;
}

/* Soundness of a CT_LETRAW node in isolation: an allocating owning op (rc/of,
 * rc/clone) must have its drop reachable before any control op (see
 * owning_dropped_before_control).  An owning `ref<T>` constructor (EX_REF) is the
 * same shape with the stricter ref discipline (ref_dropped_before_control): its
 * hoisted `drop!` must precede every control op (O1-b P1).  Struct/ADT ops
 * (make-struct, get-field, default-of) have no drop obligation and always pass. */
static bool letraw_ok(const CTerm *t) {
    if (owning_alloc_expr(t->as.letraw.e) && t->as.letraw.x.bind
        && !owning_dropped_before_control(t->as.letraw.body, t->as.letraw.x.bind->id))
        return false;
    if (t->as.letraw.e && t->as.letraw.e->kind == EX_REF && t->as.letraw.x.bind
        && !ref_dropped_before_control(t->as.letraw.body, t->as.letraw.x.bind->id))
        return false;
    return true;
}

/* Recursively check that every node lies in the C1 core subset.  Does not
 * consult the emittable set -- the cps->cps join clause is handled separately
 * by the fixpoint (needs_heap_join). */
static bool term_core_ok_impl(const CTerm *t);
/* TUR_TRACE_CORE=1: print the kind of every node the structural core check
 * rejects, innermost first -- the eviction trace's BODY-STRUCT-CORE names the
 * function, this names the form. */
static bool term_core_ok(const CTerm *t) {
    bool ok = term_core_ok_impl(t);
    if (!ok && t && getenv("TUR_TRACE_CORE"))
        fprintf(stderr, "[CORE-FAIL] kind=%d%s\n", (int)t->kind,
                t->kind == CT_UNSUPPORTED && t->as.unsupported.why
                    ? t->as.unsupported.why : "");
    return ok;
}
static bool term_core_ok_impl(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            return t->as.appcont.kont.kind != KK_PROMPT && atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return (slot_ok_t(t->as.letval.x.type, t->as.letval.x.ty) || t->as.letval.x.ty == TY_NIL)
                && atom_ok(&t->as.letval.v)
                && term_core_ok(t->as.letval.body);
        case CT_LETPRIM: {
            if (!shape_supported(t->as.letprim.spec)) return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return term_core_ok(t->as.letprim.body);
        }
        case CT_LETCALL: {
            /* The result binder may be a non-slot local (e.g. a struct/ADT read
             * only via get-field); any crossing use is gated by atom_ok below /
             * fn_sig_ok, so no slot check on the binder itself.  A CT_LETCALL is
             * always a cps->direct call to an uncolored callee, so a by-value ADT
             * arg must ride the callee's carrier ABI -- reject it (call_arg_ok
             * cps_to_direct=true) so the function evicts rather than emit a
             * raw-struct-for-int64 arg. */
            if (!call_args_ok(t->as.letcall.fn, t->as.letcall.args, t->as.letcall.n, true))
                return false;
            return term_core_ok(t->as.letcall.body);
        }
        case CT_LETRAW:
            /* An owning-value op delegated to the direct emitter.  Its operand is
             * atomic (guaranteed at translation) and its result is a local that
             * never crosses a slot, so no slot_ty check applies to the binder.
             * BUT an allocating/incrementing op (rc/of, rc/clone) creates a drop
             * obligation.  An abortive `shift` discards its continuation -- so if
             * the balancing drop is not on a straight-line path before the next
             * control op, it would be discarded (leak) or the value would abort
             * past the code that uses it (miscompile).  Require the drop up front;
             * otherwise fall back. */
            return letraw_ok(t) && term_core_ok(t->as.letraw.body);
        case CT_TAILCALL: {
            /* KK_RET / KK_VAR / KK_PROMPT are all valid here: a KK_PROMPT tail
             * call threads the enclosing prompt chain (a reset's delimited
             * body); a KK_VAR is only reifiable when the callee is not emittable
             * (handled by needs_heap_join).  A cps->direct tail call (callee not
             * CPS-emitted) passes its args RAW to the callee's direct-emitter C
             * signature, so a by-value ADT arg must ride the carrier ABI -- reject
             * it (evict).  A cps->cps tail call threads through `f__cps`, whose
             * params the CPS backend emits with the matching ABI, so the wider
             * atom_ok admits the by-value aggregate there. */
            bool cps_to_direct = !binding_cps_reachable(t->as.tailcall.fn);
            return call_args_ok(t->as.tailcall.fn, t->as.tailcall.args,
                                t->as.tailcall.n, cps_to_direct);
        }
        case CT_LETCONT:
            return (slot_ok_t(t->as.letcont.param.type, t->as.letcont.param.ty) || t->as.letcont.param.ty == TY_NIL)
                && term_core_ok(t->as.letcont.jbody)
                && term_core_ok(t->as.letcont.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && term_core_ok(t->as.if_.then_)
                && term_core_ok(t->as.if_.else_);
        case CT_MATCH:
            /* B4: an N-way tag dispatch on a heap-ADT carrier scrutinee.  The
             * scrutinee must be slot-representable (it is read as `->tag` /
             * `->as.Ctor._N`) and every arm body core-emittable.  The arm field
             * bindings are extracted inline at emit (no cross-slot check needed --
             * a binding that DOES cross into a lifted continuation rides that
             * frame's env via collect_caps, exactly like a `let` binder). */
            if (!atom_ok(&t->as.match.scrut)) return false;
            for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                if (!term_core_ok(t->as.match.arms[i].body)) return false;
            return true;
        case CT_RESET: {
            /* C3/N6.3: delimited body + a self-contained continuation whose
             * captures (if any) are all scalar source vars (carried in the env). */
            CapSet cs;
            return (slot_ok_t(t->as.reset.x.type, t->as.reset.x.ty) || t->as.reset.x.ty == TY_NIL)
                && delim_ok(t->as.reset.delim)
                && reset_body_ok(t->as.reset.body)
                && collect_caps(t->as.reset.body, t->as.reset.x.id, &cs);
        }
        case CT_SHIFT: {
            /* C3/N6.3: straight-line shift body (which already applies the
             * receiver -- cps_shift_body).  The captured continuation is
             * discarded here (abortive); resume lands in C4.  The body's scalar
             * captures (if any) ride the shift-body helper's env. */
            CapSet scs;
            return shift_body_ok(t->as.shift.body)
                && collect_caps(t->as.shift.body, t->as.shift.k.id, &scs);
        }
        case CT_HANDLE: {
            /* C4/N6.2/N6.3: N-case handler; each case <=1 effect param + zero-
             * capture straight-line body; the continuation may carry scalar
             * captures in its env. */
            /* The handled body (handle.delim) runs under the handle's OWN prompt,
             * which emit_handle threads as cur_k; its normal-completion tail AND any
             * interior COMPUTATION join deliver to that prompt (KK_PROMPT).  Admit it
             * via handle_delim_ok -- like term_core_ok but permitting the KK_PROMPT
             * delivery of a pure-computation join, so a handled expression that
             * computes its value through a join before the prompt, e.g.
             * `(+ 10 (inner))` lowering to
             * `letcont j(t){+10 t; <prompt>} in tailcall inner(j)`, is admitted.  It
             * does NOT relax interior CONTROL ops (a direct `(perform ...)` /
             * nested `handle` / `reset` in the handled body) to prompt delivery --
             * those still take the stricter term_core_ok, which the top-level
             * emit_handle cannot lower for a delimited-prompt-delivering interior
             * control op (verified: relaxing them regresses effect-error-codes /
             * effect-handler-capture-nested / handle-effectful-fn-param-same-fn).
             * The handle CONTINUATION (handle.body) delivers to the enclosing return
             * and keeps the reset_body_ok check. */
            CapSet hcs;
            if (!(slot_ok_t(t->as.handle.x.type, t->as.handle.x.ty) || t->as.handle.x.ty == TY_NIL)
                || !handle_delim_ok(t->as.handle.delim)
                || !reset_body_ok(t->as.handle.body)
                || !collect_caps(t->as.handle.body, t->as.handle.x.id, &hcs))
                return false;
            for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++) {
                const CHandleCase *c = &t->as.handle.cases[ci];
                CapSet ccs;
                /* A multi-arg effect delivers its args heap-packed as one word
                 * (emit_perform); the handler case unpacks each param from the
                 * array (emit_lifted).  Every param must be slot-representable so
                 * the pack/unpack round-trips.  A single (or zero) param keeps the
                 * original direct-in-`arg` path unchanged. */
                if (c->n_params > 1)
                    for (uint32_t pi = 0; pi < c->n_params; pi++)
                        if (!slot_ok_t(&c->params[pi]->type, c->params[pi]->type.kind))
                            return false;
                if (!handle_case_ok(c->case_body)) {
                    if (getenv("TUR_TRACE_CORE")) {
                        fprintf(stderr, "[CORE-FAIL] handle case %u: handle_case_ok; body kinds:", ci);
                        for (const CTerm *w = c->case_body; w; ) {
                            fprintf(stderr, " %d", (int)w->kind);
                            switch (w->kind) {
                                case CT_LETVAL: w = w->as.letval.body; break;
                                case CT_LETPRIM: w = w->as.letprim.body; break;
                                case CT_LETCALL: w = w->as.letcall.body; break;
                                case CT_LETRAW: w = w->as.letraw.body; break;
                                case CT_LETCONT: fprintf(stderr, "(jbody:"); for (const CTerm *v = w->as.letcont.jbody; v; ) { fprintf(stderr, " %d", (int)v->kind); if (v->kind == CT_LETVAL) v = v->as.letval.body; else if (v->kind == CT_LETPRIM) v = v->as.letprim.body; else if (v->kind == CT_LETCALL) v = v->as.letcall.body; else if (v->kind == CT_LETRAW) v = v->as.letraw.body; else v = NULL; } fprintf(stderr, ")"); w = w->as.letcont.body; break;
                                default: w = NULL;
                            }
                        }
                        fprintf(stderr, "\n");
                    }
                    return false;
                }
                if (!collect_caps_case(c->case_body, c, &ccs)) {
                    if (getenv("TUR_TRACE_CORE")) fprintf(stderr, "[CORE-FAIL] handle case %u: collect_caps_case\n", ci);
                    return false;
                }
            }
            return true;
        }
        case CT_PERFORM: {
            /* C4/N6.3: straight-line continuation whose captures (if any) are all
             * scalar source vars -- lifted with a real env.  A multi-arg effect
             * (n>1) is heap-packed into one word (emit_perform) and unpacked at the
             * handler case; every arg must be a slot-representable atom.  A single
             * (or zero) arg keeps the original one-word-slot path. */
            /* The continuation is admitted EITHER straight-line (perform_body_ok
             * -> LH_PERFORM_CONT value-transform) OR as a bounded multi-suspension
             * body (perform_cont_reset_ok -> LH_RESUME_CONT resume-frame, Track A:
             * a nested perform). */
            if (!perform_body_ok(t->as.perform.body)
                && !perform_cont_reset_ok(t->as.perform.body))
                return false;
            /* A `(perform (__Shift recv))` -- the desugar of a cross-function
             * `shift` (elab_effects.c) -- carries the shift RECEIVER as its single
             * argument: a `TY_FN` value (a lifted, capture-free `(fn [k] ...)`)
             * that the __Shift handler case invokes as `(recv k)` to resume the
             * captured continuation.  A bare TY_FN fails the generic atom_ok slot
             * gate, and widening atom_ok for ALL fn atoms miscompiles the effectful-
             * callback set (see docs/archive/cps-native-handle-in-reset-plan.md
             * "Dead ends").  Admit it ONLY here, scoped to the __Shift effect:
             * emit_perform stores the fn-pointer word as the effect value and the
             * handler case (emit_lifted) bridge-wraps the DK subk before the call,
             * so the receiver resumes the DK chain through __dk_cont_fn. */
            /* cps-dk-multishot-user-effects (Phase A): a resumable-payload user
             * effect carries its boxed-fn payload the same way __Shift carries its
             * receiver -- a `TY_FN` atom that fails the generic slot gate.  Admit it
             * at the perform-arg gate (scoped to the boxed payload, never the
             * generic atom_ok), paired with the handler-case cloneable-wrap
             * (hcase->multishot).  A non-resumable effect stays on the fiber path. */
            bool fn_payload_ok = is_shift_effect(t->as.perform.effect)
                              || t->as.perform.resumable_payload;
            for (uint32_t i = 0; i < t->as.perform.n; i++)
                if (!atom_ok(&t->as.perform.args[i])
                    && !(fn_payload_ok && shift_recv_atom_ok(&t->as.perform.args[i])))
                    return false;
            CapSet cs;
            return collect_caps(t->as.perform.body, t->as.perform.x.id, &cs);
        }
        case CT_AWAIT: {
            /* F3 (cps-async): the awaited future must be a slot-representable atom.
             * The continuation is admitted EITHER as a straight-line value
             * transform (perform_body_ok -> LH_PERFORM_CONT, the F3.1 path) OR as a
             * BOUNDED full CPS continuation (await_cont_reset_ok -> LH_RESUME_CONT,
             * the F3 gap-2 path: a branch or a further sequential `await`, with a
             * statically-bounded number of suspensions and no cps->cps tail call).
             *
             * A continuation with a cps->cps tail call (a recursive await) is
             * not admitted BY DESIGN -- await_cont_reset_ok rejects it, so it
             * evicts to the direct emitter.  Lifting it would resume via dk_invoke,
             * and a READY-future inline resume recurses through dk_invoke (not a
             * tail call) in O(N) C stack -- SIGSEGV at ~100k under a 256KB stack,
             * strictly worse than the direct TCO path (which the async-rec probe
             * runs 1,000,000 deep).  Since (async fn) is synchronous on the
             * compiled path, a recursive await is always a ready future, so the
             * direct emitter's O(1) inline-readiness lowering is the correct one.
             * This is the settled decision, not an open gap.  See
             * docs/archive/cps-async-recursive-await-eviction.md. */
            if (!atom_ok(&t->as.await.fut)) return false;
            if (!perform_body_ok(t->as.await.body) && !await_cont_reset_ok(t->as.await.body))
                return false;
            CapSet cs;
            return collect_caps(t->as.await.body, t->as.await.x.id, &cs);
        }
        case CT_RESUME:
            return t->as.resume.k.kind == CA_VAR && atom_ok(&t->as.resume.v)
                && term_core_ok(t->as.resume.body);
        case CT_CLONEABLE:
            /* U3: cloneable with a named uncolored global receiver (checked at
             * translation).  Shape 1 has no context; Shape 2's operand atom
             * (n_frames >= 1) must each be slot-representable (they ride the frame
             * env).  Admission needs a slot-representable result + core body. */
            if (!(slot_ok_t(t->as.cloneable.x.type, t->as.cloneable.x.ty) || t->as.cloneable.x.ty == TY_NIL))
                return false;
            for (uint32_t i = 0; i < t->as.cloneable.n_frames; i++) {
                const CloneFrame *fr = &t->as.cloneable.frames[i];
                /* Arithmetic frames ride their operand in the frame-env slot, so it
                 * must be a slot-atom -- UNLESS it is a non-atomic pure operand on
                 * env_expr (D6a), which is emit_value'd at the reset site like a
                 * call-frame env.  A CALL frame's captured env is likewise validated
                 * and marshaled by the builder/emitter (int/cstr inline, Serializable
                 * via its instance, or a non-atomic env on env_expr), so it does not
                 * go through the operand-slot check. */
                if (!fr->call_fn && !fr->env_expr && !atom_ok(&fr->operand)) return false;
            }
            /* An outward receiver must itself be CPS-emitted -- checked in the
             * classification fixpoint (outward_receivers_in_s), not here: this
             * runs as each entry is built, before a receiver defined later in
             * the file has an entry to ask. */
            return term_core_ok(t->as.cloneable.body);
        case CT_CALLCC:
            /* U7: a local setjmp escape landing (emit_callcc).  The receiver is
             * capture-free (build_callcc) and emitted as a value; admission needs a
             * slot-representable call/cc result and a core continuation body. */
            return (slot_ok_t(t->as.callcc.x.type, t->as.callcc.x.ty) || t->as.callcc.x.ty == TY_NIL)
                && term_core_ok(t->as.callcc.body);
        case CT_LOOP: {
            /* cps-while-native: the loop-carried params + entry atoms must be
             * slot-representable, and the loop body (a CT_IF around the interior
             * handle + back-edge) admissible. */
            for (uint32_t i = 0; i < t->as.loop.n_params; i++)
                if (!(slot_ok_t(t->as.loop.params[i].type, t->as.loop.params[i].ty)
                      || t->as.loop.params[i].ty == TY_NIL))
                    return false;
            for (uint32_t i = 0; i < t->as.loop.n_params; i++)
                if (!atom_ok(&t->as.loop.inits[i])) return false;
            return term_core_ok(t->as.loop.body);
        }
        case CT_CONTINUE:
            /* The back-edge args ride the recursive `__cps` call like tail-call
             * args -- each must be slot-representable. */
            for (uint32_t i = 0; i < t->as.cont_.n; i++)
                if (!atom_ok(&t->as.cont_.args[i])) return false;
            return true;
        default: /* CT_UNSUPPORTED */
            return false;
    }
}

/* ---- reset-delim admission (U1) ------------------------------------------
 * A reset's *delimited body* is emitted (emit_reset) with the enclosing prompt
 * chain as `cur_k`, so its tail positions deliver the delimited value to the
 * prompt (emit_deliver: `return dk_run(cur_k, v)`), and an interior join
 * (KK_VAR) delivers via goto.  That is exactly the delivery `term_core_ok`
 * forbids in *core* position (its CT_APPCONT case rejects KK_PROMPT), which is
 * why a reset body with control flow around the shift -- `(reset (if c (shift
 * ...) v))`, `(reset (+ e (if c (shift ...) v)))` -- was evicted even though
 * both the shifting and the normal-return branch are individually emittable.
 *
 * delim_ok mirrors term_core_ok but admits KK_PROMPT/KK_VAR delivery of the
 * delimited value at every tail position, recursing through the straight-line
 * (letval/letprim/letcall/letraw), branch (if), and join (letcont) structure.
 * A nested `shift` keeps its existing straight-line admission (shift_body_ok +
 * scalar-capture collect).
 *
 * CT_LETCONT (a join whose jbody may deliver to the prompt) IS admitted: it is
 * the "non-empty delimited continuation that an abortive shift discards" --
 * `(reset (+ 10 (if c (shift ...) 5)))` builds `letcont j(t){+10; prompt}` and
 * the shift branch discards j.  The CPS path lowers this correctly (the shift
 * aborts, ignoring +10); the direct/legacy path now lowers it correctly too
 * (emit_cps_reset's setjmp/longjmp escape path -- see emit_cps.c), so direct ==
 * cps holds and there is no backend divergence.
 *
 * A nested delimiter (reset/handle/perform/resume) still falls through to
 * term_core_ok, so delivering an *outer* prompt through an inner delimiter
 * stays out of scope. */
static bool delim_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            /* Deliver the delimited value to the enclosing prompt (KK_PROMPT,
             * the tail) or to an interior inline join (KK_VAR).  Both are
             * emitted by emit_deliver; unlike core position, KK_PROMPT here is
             * the correct delimited-body result. */
            return atom_ok(&t->as.appcont.v);
        case CT_LETVAL:
            return (slot_ok_t(t->as.letval.x.type, t->as.letval.x.ty) || t->as.letval.x.ty == TY_NIL)
                && atom_ok(&t->as.letval.v)
                && delim_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec)) return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return delim_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return delim_ok(t->as.letcall.body);
        case CT_LETRAW:
            return letraw_ok(t) && delim_ok(t->as.letraw.body);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && delim_ok(t->as.if_.then_)
                && delim_ok(t->as.if_.else_);
        case CT_LETCONT:
            /* A join in the delim: its jbody may deliver to the prompt (a
             * discarded delimited continuation).  Recurse via delim_ok so that
             * prompt delivery is admitted in both the join body and the term. */
            return (slot_ok_t(t->as.letcont.param.type, t->as.letcont.param.ty) || t->as.letcont.param.ty == TY_NIL)
                && delim_ok(t->as.letcont.jbody)
                && delim_ok(t->as.letcont.body);
        case CT_TAILCALL: {
            /* A KK_PROMPT tail call threads the prompt chain (same as term_core_ok). */
            for (uint32_t i = 0; i < t->as.tailcall.n; i++)
                if (!atom_ok(&t->as.tailcall.args[i])) return false;
            return true;
        }
        case CT_SHIFT: {
            /* An abortive shift in the delimited body: same admission as core. */
            CapSet scs;
            return shift_body_ok(t->as.shift.body)
                && collect_caps(t->as.shift.body, t->as.shift.k.id, &scs);
        }
        case CT_HANDLE: {
            /* A `handle` nested inside the enclosing delimited body (e.g.
             * `(reset (+ 100 (handle (use-e) (E [] k) (resume k 5))))`).  Its
             * handled body runs under the handle's own prompt (term_core_ok, as
             * in the core CT_HANDLE case), and its CASES resume/deliver to that
             * prompt (handle_case_ok) -- unchanged.  What differs from the core
             * case: the handle's CONTINUATION (handle.body, the `(+ 100 _)` that
             * consumes the handle result) sits in the enclosing delimited region,
             * so its tail delivers the value to the ENCLOSING reset's prompt
             * (KK_PROMPT) -- exactly the delivery the stricter reset_body_ok (via
             * term_core_ok) forbids, which is why a handle-in-reset used to evict.
             * Admit the continuation via delim_ok (it may deliver to a prompt),
             * with scalar-only captures riding the lifted continuation env.
             * emit_handle already lifts handle.body as an LH_RESUME_CONT that
             * delivers through cur_k (the enclosing prompt), so no new codegen is
             * needed for this (receiver-free) shape. */
            /* The handled body (handle.delim) runs under the handle's OWN prompt;
             * its normal-completion tail -- and any interior join -- delivers to
             * that prompt (KK_PROMPT), which emit_handle threads as cur_k.  So it
             * is admitted via delim_ok (KK_PROMPT-delivering), not term_core_ok:
             * a delimited-body join like `letcont j(t){+10 t; <prompt>}` inside the
             * handled expression `(+ 10 (inner))` is exactly this shape.  (In
             * Reduction A the delim was a plain `tailcall use-e(<prompt>)`, which
             * delim_ok also admits, so this is a strict widening.) */
            CapSet hcs;
            if (!(slot_ok_t(t->as.handle.x.type, t->as.handle.x.ty) || t->as.handle.x.ty == TY_NIL)
                || !delim_ok(t->as.handle.delim)
                || !delim_ok(t->as.handle.body)
                || !collect_caps(t->as.handle.body, t->as.handle.x.id, &hcs))
                return false;
            for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++) {
                const CHandleCase *c = &t->as.handle.cases[ci];
                CapSet ccs;
                if (c->n_params > 1)
                    for (uint32_t pi = 0; pi < c->n_params; pi++)
                        if (!slot_ok_t(&c->params[pi]->type, c->params[pi]->type.kind))
                            return false;
                if (!handle_case_ok(c->case_body)) return false;
                if (!collect_caps_case(c->case_body, c, &ccs)) return false;
            }
            return true;
        }
        case CT_RESET: {
            /* U7-reset (nested-reset slice): a `reset` inside the enclosing
             * delimited body.  emit_reset installs a fresh prompt (the DK machine
             * is multi-prompt; a shift binds to the nearest enclosing prompt, so
             * the nesting is correct), threads the inner delimited body under it,
             * and lifts the inner reset's continuation as a RESET_CONT that
             * delivers the nested value onward -- in tail position, to the OUTER
             * prompt (KK_PROMPT).  That outward KK_PROMPT delivery is exactly what
             * the stricter reset_body_ok (via term_core_ok) forbids, which is why
             * a nested reset used to evict to the direct emitter.  Admit the inner
             * delimited body AND its continuation via delim_ok (both may deliver
             * to a prompt), with scalar-only captures riding the lifted env. */
            CapSet cs;
            return (slot_ok_t(t->as.reset.x.type, t->as.reset.x.ty) || t->as.reset.x.ty == TY_NIL)
                && delim_ok(t->as.reset.delim)
                && delim_ok(t->as.reset.body)
                && collect_caps(t->as.reset.body, t->as.reset.x.id, &cs);
        }
        default:
            /* Nested handle/perform/resume/unsupported: keep the stricter core
             * admission -- not relaxed by this slice. */
            return term_core_ok(t);
    }
}

/* cps-dk-multishot-user-effects (Phase B): admission for a TOP-LEVEL `handle`'s
 * handled body (handle.delim).  emit_handle installs the handle's own prompt as
 * cur_k and threads the handled body under it, so the body's normal-completion
 * tail -- and any interior COMPUTATION join -- delivers its value to that prompt
 * (KK_PROMPT).  This is like term_core_ok but permits that KK_PROMPT delivery for
 * pure-computation / join / call shapes: a handled expression `(+ 10 (inner))`
 * lowers to `letcont j(t){+10 t; <prompt>} in tailcall inner(j)`, which term_core_ok
 * rejects (its CT_APPCONT forbids KK_PROMPT) but emit_handle emits correctly.
 *
 * It deliberately does NOT relax an interior CONTROL op (a direct `(perform ...)`,
 * a nested `handle`/`reset`/`shift`/`await` in the handled body) to prompt
 * delivery -- those fall to term_core_ok, which the top-level emit_handle cannot
 * lower when they would deliver through the prompt (relaxing them regresses
 * effect-error-codes / effect-handler-capture-nested / handle-effectful-fn-param-same-fn).
 * The nested-in-a-reset handle keeps its own (delim_ok) admission unchanged. */
static bool handle_delim_ok(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            return atom_ok(&t->as.appcont.v);   /* KK_PROMPT / KK_VAR delivery OK */
        case CT_LETVAL:
            return (slot_ok_t(t->as.letval.x.type, t->as.letval.x.ty) || t->as.letval.x.ty == TY_NIL)
                && atom_ok(&t->as.letval.v)
                && handle_delim_ok(t->as.letval.body);
        case CT_LETPRIM:
            if (!shape_supported(t->as.letprim.spec)) return false;
            for (uint32_t i = 0; i < t->as.letprim.n; i++)
                if (!atom_ok(&t->as.letprim.args[i])) return false;
            return handle_delim_ok(t->as.letprim.body);
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++)
                if (!atom_ok(&t->as.letcall.args[i])) return false;
            return handle_delim_ok(t->as.letcall.body);
        case CT_LETRAW:
            /* A delegated (direct-emitted) call in the handled body performs its
             * effects on the FIBER runtime, NOT the handle's DK prompt, so a
             * `(handle (f) ...)` whose body is `let x = f() [direct]; <prompt> x`
             * must NOT be admitted when `f` is EFFECTFUL (E would be unhandled --
             * regresses handle-effectful-fn-param-same-fn).  But a PURE delegated
             * op -- a call to an effect-free callee, or a raw rc/field/struct op --
             * runs no effect at all, so nothing escapes to the fiber and the
             * subsequent KK_PROMPT delivery stays the handle's own result: admit it
             * (letraw_effect_free), recursing via handle_delim_ok so the join-to-
             * prompt is allowed.  An EFFECTFUL or owning raw op falls to
             * term_core_ok (identical to the historical no-case fall-through),
             * which rejects the delivery so the handler evicts to the fiber path.
             * `(println (add-int 3 4))` inside a handled body is exactly the pure
             * case this admits. */
            if (letraw_ok(t) && letraw_effect_free(t))
                return handle_delim_ok(t->as.letraw.body);
            return term_core_ok(t);
        case CT_IF:
            return atom_ok(&t->as.if_.cond)
                && handle_delim_ok(t->as.if_.then_)
                && handle_delim_ok(t->as.if_.else_);
        case CT_LETCONT:
            return (slot_ok_t(t->as.letcont.param.type, t->as.letcont.param.ty) || t->as.letcont.param.ty == TY_NIL)
                && handle_delim_ok(t->as.letcont.jbody)
                && handle_delim_ok(t->as.letcont.body);
        case CT_TAILCALL: {
            bool cps_to_direct = !binding_cps_reachable(t->as.tailcall.fn);
            return call_args_ok(t->as.tailcall.fn, t->as.tailcall.args,
                                t->as.tailcall.n, cps_to_direct);
        }
        case CT_HANDLE: {
            /* A NESTED handle in this handle's handled body (e.g. `(handle (handle
             * (do-write-read) (Read ..)) (Write ..))`: do-write-read performs Read
             * -- handled by the inner handle -- and Write, which propagates to the
             * outer).  The inner handle runs under its OWN prompt, so its delim +
             * cases take the strict checks (via term_core_ok's own CT_HANDLE gate);
             * but its normal-completion CONTINUATION delivers the inner result to
             * THIS (enclosing) handle's prompt (KK_PROMPT), so admit the inner
             * body via handle_delim_ok -- the same computation-join-to-prompt
             * relaxation the top-level delim gets.  term_core_ok(CT_HANDLE)'s
             * `reset_body_ok(body)` rejects that KK_PROMPT delivery, which is why
             * the default->term_core_ok path below evicts an otherwise-emittable
             * nested handle. */
            CapSet hcs;
            if (!(slot_ok_t(t->as.handle.x.type, t->as.handle.x.ty) || t->as.handle.x.ty == TY_NIL)
                || !handle_delim_ok(t->as.handle.delim)
                || !handle_delim_ok(t->as.handle.body)
                || !collect_caps(t->as.handle.body, t->as.handle.x.id, &hcs))
                return false;
            for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++) {
                const CHandleCase *c = &t->as.handle.cases[ci];
                CapSet ccs;
                if (c->n_params > 1)
                    for (uint32_t pi = 0; pi < c->n_params; pi++)
                        if (!slot_ok_t(&c->params[pi]->type, c->params[pi]->type.kind))
                            return false;
                if (!handle_case_ok(c->case_body)) return false;
                if (!collect_caps_case(c->case_body, c, &ccs)) return false;
            }
            return true;
        }
        default:
            /* Interior control op / anything else: stricter core admission (no
             * KK_PROMPT delivery relaxation). */
            return term_core_ok(t);
    }
}

/* A source parameter's raw C name (used unchanged now that fn_params is set
 * during CPS emission) must not collide with a name the CPS backend synthesizes:
 * the continuation `k`, or any `__`-prefixed internal (`__root`, `__r`, `__cap`,
 * `__h<N>`, and the fresh result temporaries `__t<N>`).
 *
 * `k` itself no longer collides -- the continuation parameter has been
 * `__kont` since this rule was written -- but the rule is load-bearing anyway:
 * it keeps every colored function with a `k` parameter on the direct path, and
 * lifting it moved Saffron self-applying functions (`(defn pick [n k] ...
 * (k (- n 1) k))`) onto the CPS path, whose trampoline-root entry does not
 * release a lambda env the direct path frees (saffron-lambda-arg-env-freed
 * leaked).  So it is lifted only for a `serial-cont` parameter -- a
 * serial-shift receiver written the way the guides write one, `(defn recv
 * [k : serial-cont] ...)`, which must be CPS-emitted for its effects to reach
 * the reset's handlers (serial-receiver-effect-cannot-reach-enclosing-handler).
 * A colliding param would
 * shadow or be shadowed by a generated identifier; exclude such a function from
 * CPS candidacy so it falls back to the direct emitter (which owns its own
 * naming).  The `t<N>` branch below is retained defensively: temporaries are now
 * `__t<N>` (caught by the `__` rule), but an un-prefixed `t<N>` param remains
 * cheap to keep off the CPS path. */
static bool param_name_clashes_cps(const Binding *b) {
    if (!b || !b->name || !b->name->name) return false;
    const char *n = b->name->name;
    if (strcmp(n, "k") == 0)
        return !(b->type.kind == TY_CONT && b->type.as.cont.flavor == CONT_SERIAL);
    /* fn-value-fat-normalization (effect-row increment): a lifted capturing
     * lambda's env param is `__env_p_<id>` -- uniquely numbered, never a name
     * the CPS emitter mints itself.  Admitting it is what lets a capturing
     * callback get a `__cps` twin (the registry's slot-0 env-taking entry). */
    if (strncmp(n, "__env_p_", 8) == 0) return false;
    if (n[0] == '_' && n[1] == '_') return true;
    if (n[0] == 't' && n[1] != '\0') {
        for (const char *p = n + 1; *p; p++) if (*p < '0' || *p > '9') return false;
        return true;   /* t followed by all digits */
    }
    return false;
}

/* E1 (cps-dk-sole-effect-lowering): a fn with a SINGLE concrete C signature --
 * NOT a typeclass instance method (`owner_instance` -- those route an existential-
 * witness param BY POINTER via exwit_inst_param_by_ptr, and their base name is
 * reached through the per-method dict ABI), NOT a dict-clone (`n_dict_clone` -- a
 * rank-2 carrier value), and constraint-free (`n_constraints == 0` -- a
 * constrained/generic fn whose base name is ALSO specialized as an int64 carrier).
 * These three are exactly the fns the sig_slot_ok name-collision hazard guards
 * against: their concrete `__cps` signature would clash with the uniform
 * carrier/dict spelling of the same base name.  A plain concrete defn has one C
 * signature the direct emitter also emits, so a non-scalar signature position is
 * ABI-safe there. */
static bool fn_single_concrete_sig(const FnDef *fd) {
    return !fd->owner_instance && !fd->n_dict_clone
        && !fd->constraints.n_constraints;
}

/* E1, by-value-aggregate PARAM: admit an owning-free by-value aggregate param
 * (`slot_box_ty` -- a flat product with no rc/ref/weak field), matching the direct
 * emitter's by-value spelling (`static int64_t run(tur_adt_Cfg c)`).  Requires a
 * single concrete signature -- the `__cps` entry, its forward decl, and the
 * direct wrapper all spell it identically through emit_params.  (This also used
 * to require --enable=cps-tramp-resume, which graduated 2026-07-19.) */
static bool fn_byval_agg_param_ok(const FnDef *fd, const Binding *p) {
    /* The direct emitter passes SOME aggregates BY POINTER (`const tur_adt_H *` --
     * a defdata record ADT, a large by-value product; type_struct_pass_by_ptr),
     * not by value.  emit_params spells the CPS param BY VALUE (`tur_adt_H`), so
     * admitting a by-pointer aggregate makes the `__cps` entry / wrapper disagree
     * with the direct emitter's forward decl -- a `conflicting types` C error
     * (conv-adt-record-typed-fn-field-call).  Restrict this admission to an
     * aggregate the direct emitter ALSO passes by value (a defstruct-lowered
     * `tur_adt_Cfg`), so all three spellings match through emit_params. */
    return fn_single_concrete_sig(fd)
        && slot_box_ty(&p->type) && !type_struct_pass_by_ptr(p->type);
}

/* E2b soundness gate: the fat-closure (is_poly_fn) param `p` is only CALLED (as a
 * callee, possibly captured into a continuation env and called there), never used
 * as a VALUE -- passed as a call ARGUMENT, stored, or bare-referenced.  A fat
 * closure (`tur_poly_fn_t`) is a multi-word aggregate that cannot cross a one-word
 * slot, so a value-use would spill it to a `void *` temp and miscompile (the
 * caller/callee is_poly_fn ABI can also diverge); a callee-use goes through a
 * delegated indirect call (CT_LETRAW) or a captured `tur_poly_fn_t` env field
 * (cap_add), neither of which crosses a slot.  ptc_walk's `val` counts exactly the
 * value-uses, so `val == 0` is the sound admission condition. */
static bool fatparam_only_called(const FnDef *fd, const Binding *p) {
    int val = 0, tailc = 0, ntc = 0;
    ptc_walk(fd->body, p, true, &val, &tailc, &ntc);
    return val == 0;
}

/* E1, heap-ADT/struct HANDLE return: admit a carrier-handle return (`(Vec int)` ->
 * `tur_adt_Vec__int *`, an owning int64 carrier that crosses the DK slot by a plain
 * `(T *)__r` cast, never a box).  The `__cps` entry returns `int64_t` and delivers
 * the handle THROUGH the slot -- slot_store/slot_load already carry it via
 * carrier_handle_ok (see slot_ok_t) -- so only the signature gate blocked it.  Same
 * single-concrete-signature guard + flag.  The dangerous shape (a handle CAPTURED
 * live across a control op, which would duplicate an owning pointer into a possibly
 * multi-shot env) is independently rejected by the capture gate (cap_ty_ok excludes
 * carrier handles), so a fn that captures such a handle still evicts on the capture;
 * admitting the RETURN move-out is safe. */
static bool fn_carrier_ret_ok(const FnDef *fd, const Type *rt) {
    return fn_single_concrete_sig(fd)
        && !type_has_unresolved_tyvar(rt)
        && carrier_handle_ok(rt);
}

/* B4: admit an opaque-carrier ADT PARAM -- a boxed sum-type / non-by-value ADT
 * (e.g. `Box`) whose C spelling is the `int64_t` carrier.  BOTH the direct
 * emitter's forward decl AND emit_params (binder_ctype_full -> emit_type_c_name
 * -> type_c_name) spell such a param `int64_t`, so the `__cps` ABI is consistent
 * -- the fn_sig_ok param gate was the only thing rejecting it.  A by-value
 * PRODUCT ADT (adt_byval_c_name, e.g. `tur_adt_Cfg`) is NOT this and is handled
 * by fn_byval_agg_param_ok instead.  This lets a `perform` reachable only
 * through such a param -- e.g. `pick [b : Box]`'s match-arm perform under a DK
 * handler (cps-backend-effect-under-match) -- keep the whole call chain on the
 * DK.  Same single-concrete-signature + flag guard as the other admissions. */
static bool fn_carrier_param_ok(const FnDef *fd, const Binding *p) {
    if (!fn_single_concrete_sig(fd)) return false;
    TypeKind k = p->type.kind;
    /* B8: an opaque session-channel param is a void* carrier -- emit_params spells
     * it `void*` (matching the direct emitter), so the __cps ABI is consistent. */
    if (type_is_session(k)) return true;
    if (k != TY_ADT && k != TY_APP) return false;
    /* A tyvar-carrying carrier (`(Option a)`) belongs to a generic template and
     * must sig-REJECT so it stays a mono_template -- admitting it emits a generic
     * `<fn>__cps(int64_t, ...)` that concrete callers hit with real struct args. */
    if (type_has_unresolved_tyvar(&p->type)) return false;
    return strcmp(type_c_name(p->type), "int64_t") == 0;
}

static bool threadable_has(const Binding *b);

static bool fn_sig_ok(const FnDef *fd) {
    /* Return crosses the slot (Tier A/B scalar or Tier C boxed aggregate).  A
     * nil/void return is admitted too: the body still delivers a unit (0) to the
     * return continuation, and the entry wrapper is emitted `void` (see below).
     * fn_ret_type prefers the body Type, which carries the real def a NULL-def
     * return annotation lacks.  A by-value aggregate param passes by value in C
     * directly (never rides the slot) but is admitted through the same gate -- an
     * owning-field aggregate stays out. */
    /* The return crosses the DK slot: a Tier A/B scalar (sig_slot_ok) OR a Tier C
     * owning-free by-value aggregate boxed at the boundary (slot_box_ty).  A
     * by-value aggregate PARAM is NOT widened here -- the CPS param ABI would emit
     * it by value while the direct emitter's forward decl passes it by pointer;
     * such a param keeps the function on the fallback (see the param loop below).
     * See docs/archive/cps-tier-c-effect-result-native-plan.md. */
    const Type *rt = fn_ret_type(fd);
    if (rt->kind != TY_NIL && !sig_slot_ok(rt, rt->kind) && !slot_box_ty(rt)
        && !fn_carrier_ret_ok(fd, rt)) return false;
    /* fn-value-fat-normalization (effect-row increment): a CAPTURING lambda is
     * CPS-admitted ONLY when it is in the threadable set -- i.e. it flows into
     * an effectful fn slot and needs a registered __cps twin for the E2a fat
     * dispatch.  Every other capturing lambda keeps the direct path exactly as
     * before (which carries TCO for named-let loops, the fat-dispatch spelling
     * for poly-fn captures, and every other direct-only facility the CPS body
     * render does not reproduce).  A threadable lambda with a rank-2 poly-fn
     * capture is still rejected: its env field is the 16-byte fat struct and
     * the body's `.fn/.env` dispatch has no CPS spelling here yet. */
    if (fd->closure) {
        if (!threadable_has(fd->binding)) return false;
        for (uint8_t ci = 0; ci < fd->closure->n_captures; ci++) {
            const Binding *cap = fd->closure->captures[ci];
            if (!cap || cap->is_global) continue;
            if (cap->is_poly_fn) return false;
            /* PRIMITIVE captures only: an aggregate / type-app / tyvar capture
             * (the lens `l2 : Lens` shape) may resolve by-value under a spec,
             * be read `(__env->l2).field` under a spec-suffixed env layout the
             * CPS render does not reproduce -- and a monomorph SPEC clone of
             * an admitted lambda routes through CPS emission too.  The base
             * type looks slot-safe at classification time, so a slot test is
             * not enough; whitelist the primitive kinds the report's shapes
             * actually capture.  Rejected lambdas keep the direct path; if
             * one ever flows into a genuinely threading call it aborts loudly
             * at the checked registry lookup rather than miscompiling. */
            switch (cap->type.kind) {
                case TY_INT: case TY_BOOL: case TY_FLOAT: case TY_FLOAT32:
                case TY_FLOAT64: case TY_CSTR: case TY_FN: case TY_PTR_VOID:
                    break;
                default:
                    return false;
            }
        }
    }
    for (uint32_t i = 0; i < fd->n_params; i++) {
        const Binding *p = fd->params[i];
        /* A poly fn param crosses as a fat closure (tur_poly_fn_t); a `^borrow`
         * param is a read-only handle the callee never consumes; a plain (non-
         * rank-2) *effect-free* `TY_FN` param crosses as the direct emitter's
         * function-pointer spelling (a `cfnptr` typedef, else the opaque `int64_t`
         * carrier).  All three are passed by value in C and are admitted even when
         * their type fails the scalar / by-value-aggregate slot gate -- emit_params
         * emits the matching C type, and a call *through* such a param is an
         * uncolored delegated indirect call (CT_LETRAW), which the direct emitter
         * emits with the same spelling.
         *
         * An *effectful* fn param (`(fn [..] #{E} ..)`) is ALSO admitted.  The key
         * realization: the fiber effect machine dispatches DYNAMICALLY (a global
         * handler chain), so invoking an effectful callback is a plain fiber call
         * whose effect propagates without any continuation threading.  Delegating
         * it is therefore SOUND -- the effect never touches the DK machine.  What
         * keeps a DK function from splitting is the whole-program taint: the effect
         * is performed by the (fiber) callback body, which taints it, so its
         * handler stays co-fiber, and any DK function that would itself perform
         * that effect on DK is evicted.  The DK higher-order function's own
         * continuation survives the fiber suspend (it lives on the fiber's C
         * stack).  Multi-shot resume through a callback is a hard error on BOTH
         * paths (TUR-E0201), so it is not a new hazard.  Verified across mixing
         * DK+fiber effects, continuation-after-callback, named + nested callbacks,
         * and the full suite. */
        /* A poly-fat (rank-2) closure param (`tur_poly_fn_t`) is a multi-word
         * aggregate.  The CPS backend threads it through one-word slots / mono
         * specialization paths that cannot hold the fat struct (`void* =
         * tur_poly_fn_t`), so a function taking one must EVICT to the direct
         * emitter (which owns the fat-closure ABI).  A plain effect-free `TY_FN`
         * param -- a bare function pointer that fits the int64 carrier -- stays
         * admitted below (the delegated indirect call the comment describes). */
        /* E2b (cps-tramp-resume): a rank-2 poly-fat closure param (tur_poly_fn_t)
         * is a multi-word aggregate the CPS backend cannot thread through a
         * one-word slot -- but emit_params spells it `tur_poly_fn_t` (matching the
         * direct emitter) and a fat closure that is only CALLED (a delegated
         * indirect call, CT_LETRAW) or CAPTURED BY VALUE (cap_add admits an
         * is_poly_fn env field) never crosses a slot.  Admit it under the flag for a
         * single-concrete-signature fn; the atom gates (atom_is_fat_fn in
         * call_arg_ok / atom_ok) still EVICT a fn that threads the fat closure
         * through a DK/call-arg slot. */
        if (p->is_poly_fn
            && !(fn_single_concrete_sig(fd)
                 && fatparam_only_called(fd, p))) return false;
        bool fn_param_ok = p->type.kind == TY_FN || p->is_poly_fn;
        /* serial-receiver-effect-cannot-reach-enclosing-handler: a `k :
         * serial-cont` parameter is the opaque DK chain a serial-shift
         * receiver is handed, spelled as the int64 carrier.  It is passed by
         * value and only ever resumed or marshalled through a plain runtime
         * call, never threaded through a slot -- so, like a ^borrow handle,
         * it keeps an effectful receiver CPS-emittable. */
        bool serial_k_ok = p->type.kind == TY_CONT &&
                           p->type.as.cont.flavor == CONT_SERIAL;
        if (!p->is_borrow && !fn_param_ok && !serial_k_ok
            && !sig_slot_ok(&p->type, p->type.kind)
            && !fn_byval_agg_param_ok(fd, p)
            && !fn_carrier_param_ok(fd, p)) return false;
        if (param_name_clashes_cps(p)) return false;
    }
    return true;
}

/* SR2b (G3a clone mint): does this colored fn need a MONOMORPH CLONE to be
 * CPS-emittable?  True when its own generic signature sig-rejects (tyvar
 * params/result), so only a spec resolved through concrete bindings can carry
 * its `perform` -- the island/mono-template machinery's precondition.  A
 * colored fn whose base signature is admissible is emitted directly; minting
 * a clone for it double-emits its CPS joins (same `<name>_j<N>` symbols).
 * Read by emit_abi_register_call. */
bool emit_cps_ir_colored_fn_needs_mono(const FnDef *fd) {
    /* Keyed on the SYNTACTIC effect test (cps_expr_contains_effect_op:
     * a body that itself performs / awaits / shifts -- NOT any handle: an
     * `unsafe` block is a self-discharging handle whose fn emits fine on the
     * direct path, and minting for it broke pass-by-pointer instance params,
     * see typeclass-unsafe-passbyptr-struct-arg), not fd->cps_colored -- the coloring
     * pass has not run yet when the ABI scan asks, and running it early
     * perturbs emission state other emitters read (derive-show's pbp reads
     * broke under an early cps_color_program).  The syntactic test is the
     * coloring seed anyway, so it can only over-approximate transitively
     * colored callers -- which never reach here, because a caller without its
     * own effect form fails this test and needed no island. */
    if (!fd || !fd->body) return false;
    if (!cps_expr_contains_effect_op(fd->body)) return false;
    return !fn_sig_ok(fd);
}

/* ---- whole-program classification cache ------------------------------ */

/* `eff_lo`/`eff_hi` are a 128-bit set of the effect tags this function performs
 * or handles (bit `effect_tag(sym)-2`).  `in_s` is whether the function is
 * CPS-emitted; a non-candidate (main / exported / fell-back) is kept in the
 * table with in_s=false so its effects taint any CPS peer sharing them. */
typedef struct {
    const FnDef *fd; const Binding *bind; CTerm *term; bool in_s;
    uint64_t eff_lo, eff_hi;
    /* Split of eff_* into effects this fn PERFORMS vs HANDLES, and its colored
     * call graph -- backing the call-path taint (below).  `edges` is a bitset
     * over g_ents indices of directly-called colored peers; `edges_all` marks a
     * fn that makes an indirect/over-cap call (conservatively reaches every
     * colored peer). */
    uint64_t perf_lo, perf_hi;
    uint64_t hand_lo, hand_hi;
    uint64_t *edges;   /* NULL until the call graph is built (see ensure_S) */
    bool      edges_all;
    /* G3b: a colored GENERIC template whose monomorphs are all CPS-admissible.
     * It sig-rejects itself (tyvar TY_APP) so in_s stays false and it is never
     * emitted directly, but while mono_template is true it does NOT taint its
     * effects -- its monomorphs stand in for it, and they emit DK.  The taint
     * fixpoint evicts it (mono_template=false, reverting to tainting) if its
     * effect is tainted by a genuine fiber peer. */
    bool mono_template;
    /* PERMANENT SIG routing: this fn can NEVER be CPS-admitted -- an exported C
     * symbol, the `main` entry, or an ABI-incompatible signature (`!fn_sig_ok`:
     * a poly-fat / by-value-aggregate param the DK ABI cannot spell).  Distinct
     * from a `!term_core_ok` BODY reason, which IS fixable admission work.  Backs
     * the SIG-cascade trace category: an effect touched by a sig_perm fn is
     * PERMANENTLY tainted, so a fn evicted only because it shares such an effect
     * is itself permanent routing (no BODY-* fix exists), not a fixable BODY root. */
    bool sig_perm;
    /* The CPS IR's fresh binders in `term` are `__t0` .. `__t<fresh_n-1>`, the
     * same spelling as the direct emitter's fresh_tmp; rendering the term
     * raises ctx->tmp_n to at least this first. */
    uint32_t fresh_n;
} SEnt;

/* The program entry point `main` (never module-prefixed). */
static bool fn_is_main(const FnDef *fd) {
    return fd && fd->binding && fd->binding->name
        && fd->binding->name->len == 4
        && memcmp(fd->binding->name->name, "main", 4) == 0;
}

/* cps-backend-direct-lowering-removal-plan (D2b): is this a `main` that should
 * be CPS-emitted with a fixed-ABI entry wrapper?  A zero-arg `main` whose body
 * directly uses delimited control (reset/shift/shift0, cloneable/serial reset)
 * is otherwise excluded from S purely because of its entry-point ABI; admitting
 * it (subject to the same fn_sig_ok/term_core_ok subset gates) lets its delimited
 * ops emit through the native CT-IR path instead of the direct emitter, and the
 * `int main(...)` wrapper trampolines into `main__cps`.  Effect-only mains (no
 * delimited op) stay excluded -- CPS-emitting them is pure overhead and removes
 * no direct-lowering caller. */
/* E3': does `e` directly contain an effect `handle`?  (partial structural walk over
 * the forms a `main` body takes: a bare handle, or one wrapped in do/let/if/...).
 * A miss only leaves `main` on its historical fiber path -- conservative. */
static bool expr_has_handle(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_HANDLE: return true;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (expr_has_handle(e->as.do_.items[i])) return true;
            return false;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (expr_has_handle(e->as.let_.bindings[i].init)) return true;
            return expr_has_handle(e->as.let_.body);
        case EX_IF:
            return expr_has_handle(e->as.if_.cond) || expr_has_handle(e->as.if_.then_)
                || expr_has_handle(e->as.if_.else_or_null);
        case EX_REINTERPRET: return expr_has_handle(e->as.reinterpret_.expr);
        case EX_ASCRIBE: return expr_has_handle(e->as.ascribe_.inner);
        case EX_RETURN:  return expr_has_handle(e->as.return_.value);
        /* A `(with-handler hv body)` is a delimited handler install just like
         * `handle`.  When hv is a `(handler ...)` literal (or a compose of them)
         * build_with_handler DK-lowers it via build_handle_core; a dynamic
         * handler value evicts to CT_UNSUPPORTED and falls back gracefully under
         * the experiment.  Either way main must be a d2b candidate so the literal
         * form reaches the DK backend instead of the fiber. */
        case EX_WITH_HANDLER: return true;
        case EX_COMPOSE_HANDLERS: return true;
        default: return false;
    }
}

static bool fn_is_d2b_main(const FnDef *fd) {
    if (!(fn_is_main(fd) && fd->n_params == 0 && fd->body)) return false;
    /* E3 (v2 sole-effect-lowering): a zero-arg main gets the int-main -> main__cps
     * trampoline when it contains delimited control.  An earlier revision made
     * EVERY main d2b under cps-tramp-resume, which broke a main that transitively
     * reaches FIBER effect code (SIGSEGV / print-skipped / unhandled-effect).
     *
     * E3' (cps-tramp-resume): a main that ITSELF installs a `handle` is now a d2b
     * CANDIDATE -- NOT unconditionally d2b.  It enters the taint fixpoint like any
     * candidate; if its handle subtree reaches fiber effect code the fixpoint drops
     * it from S, and emit_cps_ir_try_fn returns false BEFORE the d2b wrapper (line
     * ~5863), so a fiber-reaching main falls back to the historical direct/fiber
     * main -- exactly its current behaviour, with NO forced-d2b regression.  The
     * gate is thus the fixpoint (the "taint-completeness guard" the note asked for),
     * unlocked now that E2a threads the effectful fn-values a handle-main performs. */
    return cps_expr_contains_shift(fd->body)
        || expr_has_handle(fd->body);
}

static const Expr *g_prog;      /* program the cache is keyed on */
/* g_emit_ctx: G3b, ctx of the active emission (declared above call_args_ok). */
static EmitCtx    *g_ents_ctx;  /* G3b: the g_emit_ctx the cached g_ents were classified under */
static Arena       g_arena;     /* owns the cached CTerms (+ coloring) */
static bool        g_arena_live;
static SEnt       *g_ents;
static size_t      g_ents_n;
/* Permanently-tainted effects: the effect set that stays fiber-tainted even when
 * every BODY-fixable function is admitted (i.e. only sig_perm fns + base code +
 * their call-path conduits are fiber).  A colored fn evicted ONLY because it
 * shares such an effect is permanent SIG-cascade routing -- no BODY-* fix could
 * admit it -- so the EVICT trace reports it as SIG-TAINT, not BODY-STRUCT-OR-TAINT.
 * Computed by ensure_S (a fixpoint over sig_perm sources); read by the trace. */
static uint64_t    g_perm_lo, g_perm_hi;
static bool        g_fwd_done;  /* __cps forward decls emitted for g_prog */

/* E2/taint-completeness: the set of global fns whose ADDRESS is taken (referenced
 * as a fn-value, not called directly).  Populated by a pre-pass over the program
 * (g_addr_collecting) and read in ensure_S to force an effectful address-taken fn
 * to the fiber (its DK direct-entry would escape a perform when invoked indirectly). */
static const Binding *g_addr_taken[1024];
static int            g_addr_taken_n;
static bool           g_addr_collecting;
static void addr_taken_add(const Binding *b) {
    for (int i = 0; i < g_addr_taken_n; i++) if (g_addr_taken[i] == b) return;
    if (g_addr_taken_n < (int)(sizeof(g_addr_taken)/sizeof(g_addr_taken[0])))
        g_addr_taken[g_addr_taken_n++] = b;
}
static bool addr_taken_has(const Binding *b) {
    for (int i = 0; i < g_addr_taken_n; i++) if (g_addr_taken[i] == b) return true;
    return false;
}

/* Effect -> prompt-tag registry.  Tags start at 2 (reset/shift use 1); the same
 * interned effect Symbol always maps to the same tag, so a handle and a matching
 * perform agree.  Reset per program. */
#define CPS_MAX_EFFECTS 128
static const Symbol *g_eff_syms[CPS_MAX_EFFECTS];
static int           g_eff_n;

/* Non-static (B3 part 2): the direct-path handler-literal emitter
 * (emit_effects_handler_lit) calls this to stamp a DK case entry's `dk_tag` with
 * the SAME per-effect integer the body's `dk_perform(tag, ...)` uses -- the tag
 * is memoized by symbol, so first-seen order is irrelevant to consistency. */
int effect_tag(const Symbol *eff) {
    for (int i = 0; i < g_eff_n; i++)
        if (g_eff_syms[i] == eff) return i + 2;
    if (g_eff_n < CPS_MAX_EFFECTS) { g_eff_syms[g_eff_n] = eff; return (g_eff_n++) + 2; }
    return 2;
}

static bool binding_in_s(const Binding *b) {
    if (!b) return false;
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].bind == b) return g_ents[i].in_s;
    return false;
}
static bool binding_cps_reachable(const Binding *b) {
    if (!b) return false;
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].bind == b) return g_ents[i].in_s || g_ents[i].mono_template;
    return false;
}

static const char *find_mono_clone_for_call(EmitCtx *ctx, const Binding *fn,
                                            const CAtom *args, uint32_t n,
                                            const Expr *call_expr);
/* colored-generic-clone-join-emitted-inline: does this CT_TAILCALL take the
 * cps->cps route (thread the continuation into `<callee>__cps`)?  ONE answer
 * for the classifier and the emitter.  The tail-arm emitter routes cps->cps
 * when the callee is in S, OR when it is a colored mono-template whose clone
 * resolves at this call (`<clone>__cps` is emitted) -- unless a typeclass
 * re-resolution or a region bracket forces the direct arm.  The join
 * classifier (letcont_is_heap_join / needs_heap_join / jbody_has_cps_tailcall)
 * used to ask only `binding_in_s`, so a bind-position call to a colored
 * GENERIC's clone -- `(not (option-eq? (some 1) (some 2) f))` once `option-eq?`
 * was colored -- was classified as an INLINE join while the emitter threaded
 * `__kont` straight into the clone: the `not` sat behind a dead label and the
 * callee's answer went to the caller's continuation unchanged.  A silent
 * wrong answer (`false`, and `1` for an `if` on it).  Every site asks this. */
static bool tailcall_routes_cps_to_cps(const CTerm *t) {
    if (!t || t->kind != CT_TAILCALL) return false;
    const Binding *fn = t->as.tailcall.fn;
    if (!fn) return false;
    if (t->as.tailcall.call_expr && g_emit_ctx) {
        char *rr = emit_reresolve_method_call(g_emit_ctx, t->as.tailcall.call_expr);
        if (rr) { free(rr); return false; }
    }
    if (emit_binding_is_region_scope(fn)) return false;
    if (binding_in_s(fn)) return true;
    SEnt *fe = NULL;
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].bind == fn) { fe = &g_ents[i]; break; }
    if (!fe || !fe->mono_template || !g_emit_ctx) return false;
    return find_mono_clone_for_call(g_emit_ctx, fn, t->as.tailcall.args,
                                    t->as.tailcall.n, t->as.tailcall.call_expr) != NULL;
}

/* True when this CT_LETCONT is a non-tail cps->cps call: its body is directly a
 * tailcall to an emittable colored callee that threads this join as the callee's
 * continuation.  Such a join is reified as a DK frame (emit_heap_join) rather
 * than an inline goto-join. */
static bool letcont_is_heap_join(const CTerm *t) {
    const CTerm *b = t->as.letcont.body;
    return b && b->kind == CT_TAILCALL
        && b->as.tailcall.kont.kind == KK_VAR
        && b->as.tailcall.kont.id == t->as.letcont.j.id
        /* colored callee threads `<fn>__cps`, (E2a tier-`nontail`) a fn-value
         * callee threads via the registry, OR (E2) a fat-closure poly-fn param
         * threads via its fn_cps slot -- all reify this join as a DK frame. */
        && (tailcall_routes_cps_to_cps(b) || b->as.tailcall.via_registry
            || b->as.tailcall.via_fncps);
}

/* A heap-join jbody is lowered into a value-transform frame fn `(env, value)`
 * that RETURNS jbody's value (emit_lifted LH_PERFORM_CONT); the DK machine then
 * delivers that return through the frame's `next` (cur_k).  The frame fn thus
 * has NO continuation in scope -- neither the enclosing `k` (KK_RET) nor a join
 * var (KK_VAR) -- so a jbody that tail-calls another colored function is
 * unemittable: the lowered `g__cps(args, k)` would reference an undeclared `k`.
 * (needs_heap_join's own CT_TAILCALL case catches a KK_VAR cps->cps call, but a
 * KK_RET cps->cps call is legitimate at a function's TOP level -- where `k` is a
 * real param -- and only unemittable once buried in a lifted join body, which is
 * exactly what this scan detects.)  A nested if/let spine is walked to its tail
 * positions; control ops and plain value deliveries carry no bare cps->cps tail
 * call. */
static bool jbody_has_cps_tailcall(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        /* E2/E2a: a fn-value tail call (via the registry, or a fat-closure fn_cps
         * slot) also threads the DK continuation, so a jbody containing one needs
         * a resume-frame (LH_RESUME_CONT) that receives `__kont` at run time. */
        case CT_TAILCALL: return tailcall_routes_cps_to_cps(t)
                              || t->as.tailcall.via_registry
                              || t->as.tailcall.via_fncps;
        case CT_LETVAL:   return jbody_has_cps_tailcall(t->as.letval.body);
        case CT_LETPRIM:  return jbody_has_cps_tailcall(t->as.letprim.body);
        case CT_LETCALL:  return jbody_has_cps_tailcall(t->as.letcall.body);
        case CT_LETRAW:   return jbody_has_cps_tailcall(t->as.letraw.body);
        case CT_IF:       return jbody_has_cps_tailcall(t->as.if_.then_)
                              || jbody_has_cps_tailcall(t->as.if_.else_);
        case CT_MATCH:    for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                              if (jbody_has_cps_tailcall(t->as.match.arms[i].body)) return true;
                          return false;
        case CT_LETCONT:  return jbody_has_cps_tailcall(t->as.letcont.jbody)
                              || jbody_has_cps_tailcall(t->as.letcont.body);
        default:          return false;
    }
}

/* A heap-join jbody that itself PERFORMS an effect also needs `__kont` in scope:
 * the interior dk_perform threads cur_k (== the frame's runtime downstream chain)
 * so the effect reaches the enclosing handler.  A value-only LH_PERFORM_CONT
 * frame fn `(env, value)` has no `__kont`, so a perform lowered inside it emits an
 * undeclared `__kont` (the compound `effect-reopen` shape:
 * `perform E; let x = colored(...); perform E2; ...`).  Detecting a perform in the
 * jbody promotes the frame to an LH_RESUME_CONT resume-frame, which RECEIVES its
 * downstream chain as the `__kont` param, exactly like the cps->cps-tailcall case.
 * See docs/archive/cps-perform-cont-heap-join-eviction.md. */
static bool jbody_has_perform(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_PERFORM:  return true;
        case CT_LETVAL:   return jbody_has_perform(t->as.letval.body);
        case CT_LETPRIM:  return jbody_has_perform(t->as.letprim.body);
        case CT_LETCALL:  return jbody_has_perform(t->as.letcall.body);
        case CT_LETRAW:   return jbody_has_perform(t->as.letraw.body);
        case CT_RESUME:   return jbody_has_perform(t->as.resume.body);
        case CT_IF:       return jbody_has_perform(t->as.if_.then_)
                              || jbody_has_perform(t->as.if_.else_);
        case CT_MATCH:    for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                              if (jbody_has_perform(t->as.match.arms[i].body)) return true;
                          return false;
        case CT_LETCONT:  return jbody_has_perform(t->as.letcont.jbody)
                              || jbody_has_perform(t->as.letcont.body);
        default:          return false;
    }
}

/* A heap-join jbody that installs a NESTED delimited-control op in VALUE position
 * (a `handle`/`reset` whose result feeds an enclosing expression, e.g.
 * `(+ x (handle ...))`) needs `__kont` in scope: the nested handle's lifted
 * continuation frame captures the enclosing continuation (`__ce->__k = __kont`),
 * so the join must ride an LH_RESUME_CONT resume-frame (which RECEIVES its
 * downstream chain as `__kont`) rather than a value-only LH_PERFORM_CONT frame
 * (which has no `__kont`).  See docs/archive/cps-toplevel-synthesized-main-
 * bypasses-dk.md (effect-nested). */
static bool jbody_has_delim(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_HANDLE:   return true;
        case CT_RESET:    return true;
        case CT_LETVAL:   return jbody_has_delim(t->as.letval.body);
        case CT_LETPRIM:  return jbody_has_delim(t->as.letprim.body);
        case CT_LETCALL:  return jbody_has_delim(t->as.letcall.body);
        case CT_LETRAW:   return jbody_has_delim(t->as.letraw.body);
        case CT_RESUME:   return jbody_has_delim(t->as.resume.body);
        case CT_IF:       return jbody_has_delim(t->as.if_.then_)
                              || jbody_has_delim(t->as.if_.else_);
        case CT_MATCH:    for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                              if (jbody_has_delim(t->as.match.arms[i].body)) return true;
                          return false;
        case CT_LETCONT:  return jbody_has_delim(t->as.letcont.jbody)
                              || jbody_has_delim(t->as.letcont.body);
        default:          return false;
    }
}

/* Does `t` contain a heap join the backend CANNOT emit?  A non-tail cps->cps
 * call reifies its join continuation as a DK frame; that is now supported for
 * the direct CT_LETCONT-body form, provided the join body is zero-capture (the
 * frame carries only the enclosing continuation).  This returns true only for
 * the cases still unhandled: a capturing heap join, a jbody that itself contains
 * a cps->cps tail call (the lifted frame fn has no `k` to thread), or a KK_VAR
 * cps->cps tail call not sitting directly under its CT_LETCONT (e.g. inside an
 * if branch). */
/* perform-inside-loop-has-no-lowering: does a delivery to join `jid` occur
 * INSIDE a region of `t` that the emitter lifts into its own C function (the
 * continuation of a perform / await / resume, a reset or handle continuation,
 * a shift / callcc / cloneable body, a loop helper)?  A label join cannot be
 * reached from there (`goto L<j>` would name a label in the parent function;
 * the join parameter is not in scope either), so such a join is reified as a
 * DK resume-frame instead -- see emit_escaping_join.  Over-approximating
 * "lifted" is safe: the frame lowering is valid from the parent frame too. */
static bool letcont_is_heap_join(const CTerm *t);
static bool letcont_is_escaping_join(const CTerm *t);
static bool join_escapes_lifted_rec(const CTerm *t, uint32_t jid, bool lifted) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT:
            return lifted && t->as.appcont.kont.kind == KK_VAR
                && t->as.appcont.kont.id == jid;
        case CT_TAILCALL:
            return lifted && t->as.tailcall.kont.kind == KK_VAR
                && t->as.tailcall.kont.id == jid;
        case CT_LETVAL:  return join_escapes_lifted_rec(t->as.letval.body, jid, lifted);
        case CT_LETPRIM: return join_escapes_lifted_rec(t->as.letprim.body, jid, lifted);
        case CT_LETCALL: return join_escapes_lifted_rec(t->as.letcall.body, jid, lifted);
        case CT_LETRAW:  return join_escapes_lifted_rec(t->as.letraw.body, jid, lifted);
        case CT_LETCONT: {
            /* A nested join that is itself reified (heap or escaping) lifts its
             * jbody into a frame, so a delivery to `jid` from there is lifted. */
            bool jl = lifted || letcont_is_heap_join(t) || letcont_is_escaping_join(t);
            return join_escapes_lifted_rec(t->as.letcont.jbody, jid, jl)
                || join_escapes_lifted_rec(t->as.letcont.body, jid, lifted);
        }
        case CT_IF:
            return join_escapes_lifted_rec(t->as.if_.then_, jid, lifted)
                || join_escapes_lifted_rec(t->as.if_.else_, jid, lifted);
        case CT_MATCH:
            for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                if (join_escapes_lifted_rec(t->as.match.arms[i].body, jid, lifted)) return true;
            return false;
        case CT_PERFORM:   return join_escapes_lifted_rec(t->as.perform.body, jid, true);
        case CT_AWAIT:     return join_escapes_lifted_rec(t->as.await.body, jid, true);
        case CT_RESUME:    return join_escapes_lifted_rec(t->as.resume.body, jid, true);
        case CT_RESET:
            return join_escapes_lifted_rec(t->as.reset.delim, jid, true)
                || join_escapes_lifted_rec(t->as.reset.body, jid, true);
        case CT_SHIFT:     return join_escapes_lifted_rec(t->as.shift.body, jid, true);
        case CT_HANDLE: {
            if (join_escapes_lifted_rec(t->as.handle.delim, jid, true)
                || join_escapes_lifted_rec(t->as.handle.body, jid, true)) return true;
            for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++)
                if (join_escapes_lifted_rec(t->as.handle.cases[ci].case_body, jid, true)) return true;
            return false;
        }
        case CT_CLONEABLE: return join_escapes_lifted_rec(t->as.cloneable.body, jid, true);
        case CT_CALLCC:    return join_escapes_lifted_rec(t->as.callcc.body, jid, true);
        case CT_LOOP:
            /* The helper's exit delivers to result_kont from its own function. */
            return (t->as.loop.result_kont.kind == KK_VAR && t->as.loop.result_kont.id == jid)
                || join_escapes_lifted_rec(t->as.loop.body, jid, true);
        default: return false;
    }
}
static bool letcont_is_escaping_join(const CTerm *t) {
    return !letcont_is_heap_join(t)
        && join_escapes_lifted_rec(t->as.letcont.body, t->as.letcont.j.id, false);
}

/* Escaping joins enclosing the term needs_heap_join is currently walking (a
 * nested escaping join may deliver to one of these through its chain). */
static uint32_t g_esc_joins[64];
static int      g_esc_join_n;

static bool needs_heap_join(const CTerm *t) {
    if (!t) return false;
    switch (t->kind) {
        case CT_APPCONT: return false;
        case CT_LETVAL:  return needs_heap_join(t->as.letval.body);
        case CT_LETPRIM: return needs_heap_join(t->as.letprim.body);
        case CT_LETCALL: return needs_heap_join(t->as.letcall.body);
        case CT_LETRAW:  return needs_heap_join(t->as.letraw.body);
        case CT_TAILCALL:
            return t->as.tailcall.kont.kind == KK_VAR
                && tailcall_routes_cps_to_cps(t);
        case CT_LETCONT:
            if (letcont_is_heap_join(t)) {
                /* The join is lifted into a DK frame; the handled tailcall (body)
                 * is not walked.  P6 (cps-runtime-finish, heap-join-over-recursion):
                 * a jbody that CAPTURES enclosing locals and/or itself makes a
                 * cps->cps tail call (e.g. `set-eq-loop`/`__cons-fmap`: call a
                 * colored helper, then recurse) is emittable -- emit_heap_join lifts
                 * it as an LH_RESUME_CONT resume-frame (dk_frame_resume) whose
                 * run-time `__kont` parameter is the downstream chain the DK passes
                 * in, so a KK_RET delivery lowers `dk_run(__kont, v)` and a recursive
                 * cps->cps tail call threads `__kont` -- delivered exactly once (the
                 * DKK_RESUME_FRAME rfn consumes its downstream).  The only remaining
                 * unemittable case is a NON-slot capture (collect_caps fails);
                 * recurse for deeper joins in the jbody. */
                CapSet _cs;
                if (!collect_caps(t->as.letcont.jbody, t->as.letcont.param.id, &_cs))
                    return true;
                /* cps-join-point-emits-invalid-assignment: the lifted frame fn
                 * has NO enclosing join in scope -- only its own `__kont` (a
                 * KK_RET delivery) and joins it defines itself.  A jbody that
                 * delivers to an OUTER join (KK_VAR) therefore has nowhere to
                 * go, and the emitter used to fall through to its
                 * "unreachable when the term is well-formed" placeholders:
                 * join_param() returned the literal "0", producing
                 *
                 *     0 = __t5;      <- not an lvalue
                 *     goto L4;      <- label lives in the PARENT function
                 *
                 * This is the same closedness requirement the lifted
                 * reset/handler bodies already carry (joins_closed_rec, whose
                 * header comment states it); heap joins were simply never
                 * checked against it.  Reject with a FRESH def set so any join
                 * reference that escapes the jbody evicts the function to the
                 * direct emitter. */
                {
                    uint32_t _jdef[CC_MAX_BOUND];
                    if (!joins_closed_rec(t->as.letcont.jbody, _jdef, 0))
                        return true;
                }
                return needs_heap_join(t->as.letcont.jbody);
            }
            if (letcont_is_escaping_join(t)) {
                /* The join is reified as a resume-frame (emit_escaping_join):
                 * its body must capture only slot values and must not deliver
                 * to an OUTER join -- except an outer ESCAPING join, which the
                 * frame reaches through its downstream chain (the outer frame
                 * was cur_k when this one was spliced).  Otherwise evict. */
                CapSet _ecs;
                if (!collect_caps(t->as.letcont.jbody, t->as.letcont.param.id, &_ecs))
                    return true;
                uint32_t _edef[CC_MAX_BOUND];
                int _end = 0;
                for (int i = 0; i < g_esc_join_n && _end < CC_MAX_BOUND; i++)
                    _edef[_end++] = g_esc_joins[i];
                if (!joins_closed_rec(t->as.letcont.jbody, _edef, _end))
                    return true;
                if (needs_heap_join(t->as.letcont.jbody)) return true;
                bool pushed = g_esc_join_n < 64;
                if (pushed) g_esc_joins[g_esc_join_n++] = t->as.letcont.j.id;
                bool r = needs_heap_join(t->as.letcont.body);
                if (pushed) g_esc_join_n--;
                return r;
            }
            return needs_heap_join(t->as.letcont.jbody)
                || needs_heap_join(t->as.letcont.body);
        case CT_IF:
            return needs_heap_join(t->as.if_.then_)
                || needs_heap_join(t->as.if_.else_);
        case CT_MATCH:
            for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                if (needs_heap_join(t->as.match.arms[i].body)) return true;
            return false;
        case CT_RESET:
            return needs_heap_join(t->as.reset.delim)
                || needs_heap_join(t->as.reset.body);
        case CT_SHIFT:
            return needs_heap_join(t->as.shift.body);
        case CT_HANDLE: {
            if (needs_heap_join(t->as.handle.delim) || needs_heap_join(t->as.handle.body))
                return true;
            for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++)
                if (needs_heap_join(t->as.handle.cases[ci].case_body)) return true;
            return false;
        }
        case CT_PERFORM:
            return needs_heap_join(t->as.perform.body);
        case CT_AWAIT:
            return needs_heap_join(t->as.await.body);
        case CT_RESUME:
            return needs_heap_join(t->as.resume.body);
        case CT_CLONEABLE:
            return needs_heap_join(t->as.cloneable.body);
        case CT_CALLCC:
            return needs_heap_join(t->as.callcc.body);
        default: return false;
    }
}

/* Mark effect `eff` in a 128-bit set (bit index = effect_tag - 2). */
static void mark_effect(const Symbol *eff, uint64_t *lo, uint64_t *hi) {
    if (!eff) return;
    int idx = effect_tag(eff) - 2;      /* >=0 */
    if (idx < 64)       *lo |= (uint64_t)1 << idx;
    else if (idx < 128) *hi |= (uint64_t)1 << (idx - 64);
}

/* Accumulator for the raw-Expr effect/call walk.  `plo/phi` collect the tags of
 * effects the expression *performs*; `hlo/hhi` collect the tags it *handles*
 * (installs a prompt for).  When `callees` is non-NULL, every direct call's
 * `fn_binding` is appended (deduped) up to `cap_callees`; an indirect call, or
 * an overflow past the cap, sets `*callee_overflow` (the caller then treats the
 * function as reaching every colored peer -- sound over-approximation).  The
 * combined "performs-or-handles" wrapper aliases plo==hlo and phi==hhi. */
/* r7rs-conformance-program-emits-megabytes-of-c: the E2 threadability tallies
 * for EVERY fn-value at once.  ensure_S used to walk the whole program once
 * per fn-value (fn_value_threadable, fnval_stored_in_struct) -- quadratic, and
 * about 40% of `emit-c` on a program of 3,800 lambdas.  The walk's dependence
 * on its single `count_target` is three spots (a value use, a closure literal,
 * a threadable call argument), so one walk with a binding -> slot index serves
 * every target, each slot tallied exactly as its own walk would have. */
typedef struct FvMulti {
    const Binding **keys;   /* open addressing; NULL = empty */
    int            *slot;   /* keys[h]'s slot */
    uint32_t        cap;    /* power of two */
    int            *total, *ok, *tier;
    bool           *stored; /* fnval_stored_in_struct, per slot */
    /* A threadable-argument use whose callee is a CAPTURING lambda: its
     * param_thread_class goes through fn_sig_ok, which asks threadable_has --
     * state the classification loop grows.  Its tier is left out of `tier` and
     * re-asked when the loop reaches the slot, as the per-target walk did. */
    const FnDef   **dyn_cfd;
    uint32_t       *dyn_pi;
    int            *dyn_next;   /* chain per slot; -1 ends it */
    int            *dyn_head;   /* per slot */
    int             dyn_n, dyn_cap;
} FvMulti;

typedef struct {
    uint64_t *plo, *phi;      /* performed effects */
    uint64_t *hlo, *hhi;      /* handled effects   */
    const Binding **callees;  /* optional: direct-call bindings (deduped)      */
    int       *n_callees;
    int        cap_callees;
    bool      *callee_overflow;
    /* E2 threadability analysis (cps-tramp-resume): when count_target is non-NULL,
     * every EX_VAR reference to it (a value-USE of that binding, anywhere in the
     * walked tree including nested lambda bodies) increments *count_out.  A direct
     * call's callee is carried in fn_binding, not as an EX_VAR node, so it is not
     * counted -- only genuine fn-VALUE uses are.  Zero-initialized by the positional
     * initializers that predate these fields (trailing omitted => zero). */
    const Binding *count_target;
    int           *count_out;
    /* E2 threadability (cps-tramp-resume): when thr_ok is non-NULL, every EX_CALL
     * whose arg[i] peels to count_target and whose global callee's param i is a
     * THREADING param increments *thr_ok.  Run in the SAME walk as count_out so
     * total (count_out) and threadable-arg (thr_ok) uses are directly comparable
     * over one exhaustive traversal (covers handle/reset/match/... uniformly). */
    const Expr    *thr_program;
    int           *thr_ok;
    /* E2 (param_is_threading): when callee_target is non-NULL, every EX_CALL whose
     * fn_binding IS it increments *callee_out.  A param called as `(p x)` carries p
     * in fn_binding (no EX_VAR node), so counting fn_binding is how a param's
     * callee-uses are tallied exhaustively.  NOT set when count_target is a global
     * fn (a direct call to it is not a fn-value use). */
    const Binding *callee_target;
    int           *callee_out;
    /* E2 tiering: when thr_tier is non-NULL, each in-surface threadable-arg use of
     * count_target raises *thr_tier to max(*thr_tier, param_thread_class) so the
     * trace can report the hardest E2 tier a fn-value needs (PT_NOW/NONTAIL/E1). */
    int           *thr_tier;
    /* letraw_effect_free: record only CALL-position callees, skipping the
     * fn-VALUE reference channel below (EX_VAR on a TY_FN binding).  The two
     * uses of the callee list ask different questions -- the call-path taint
     * asks "may this fn be called downstream through the value", which a value
     * reference genuinely answers yes to; a CT_LETRAW admission asks "does this
     * raw op run an effect right here", which taking a function's ADDRESS never
     * does.  Only letraw_effect_free sets this. */
    bool           calls_only;
    /* Multi-target E2 tallies (FvMulti, above): set in place of
     * count_target / count_out / thr_ok / thr_tier by fv_multi_tally. */
    const FvMulti *multi;
} EffAcc;

/* E2 param-threading tiers -- how ready a HOF param is to thread the DK to the
 * fn-value it binds, in ASCENDING implementation difficulty.  PT_NONE = not a
 * threading param (the fn-value escapes as a value, is called under the HOF's own
 * handle, or lives in a form the classifier does not cover -> stays fiber). */
typedef enum {
    PT_NONE = 0,   /* not threadable: value-use / handler-body / sink */
    PT_NOW,        /* tail-only callee, scalar-carrier param -- easiest E2 target */
    PT_NONTAIL,    /* callee-only but some call is non-tail -- needs a threaded
                    * CT_LETCALL for the fn-value call (E2 emission work) */
    PT_E1,         /* callee-only but the param is is_poly_fn (rank-2/capturing) --
                    * needs E1's carrier-ABI __cps before it can thread */
} PtClass;

/* Forward decls: the walker's EX_CALL threadability probe uses these, but they
 * depend on expr_count_all_uses (which drives this walker), so they are defined
 * after it. */
static const Expr  *peel_fn_value(const Expr *e);
static const FnDef *fd_for_binding(const Expr *program, const Binding *b);
static PtClass      param_thread_class(const FnDef *fd, uint32_t pi);

static int fvm_slot(const FvMulti *m, const Binding *b) {
    if (!m || !b || !m->cap) return -1;
    uint64_t x = (uint64_t)(uintptr_t)b;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    for (uint32_t h = (uint32_t)x & (m->cap - 1);; h = (h + 1) & (m->cap - 1)) {
        if (!m->keys[h]) return -1;
        if (m->keys[h] == b) return m->slot[h];
    }
}

/* The targets a fn-value reference `a` (already peeled) is a use of, exactly
 * as the single-target walk tests them: the variable's own binding, or the
 * lifted lambda a let / `^borrow`-hoist temp records, or a closure literal's
 * lifted lambda.  Deduplicated, so one reference counts once per target. */
static int fvm_ref_slots(const FvMulti *m, const Expr *a, int out[3]) {
    int n = 0;
    const Binding *c[3] = { NULL, NULL, NULL };
    if (a && a->kind == EX_VAR && a->as.var.binding) {
        c[0] = a->as.var.binding;
        c[1] = a->as.var.binding->closure_fn_binding;
        c[2] = a->as.var.binding->hoist_closure_fn_binding;
    } else if (a && a->kind == EX_CLOSURE && a->as.closure_.closure
               && a->as.closure_.closure->fn) {
        c[0] = a->as.closure_.closure->fn->binding;
    }
    for (int k = 0; k < 3; k++) {
        if (!c[k]) continue;
        bool dup = false;
        for (int j = 0; j < k; j++) if (c[j] == c[k]) dup = true;
        if (dup) continue;
        int sl = fvm_slot(m, c[k]);
        if (sl >= 0) out[n++] = sl;
    }
    return n;
}

static void fvm_dyn_add(FvMulti *m, int sl, const FnDef *cfd, uint32_t pi) {
    if (m->dyn_n == m->dyn_cap) {
        int nc = m->dyn_cap ? m->dyn_cap * 2 : 64;
        m->dyn_cfd  = (const FnDef **)realloc((void *)m->dyn_cfd, (size_t)nc * sizeof(*m->dyn_cfd));
        m->dyn_pi   = (uint32_t *)realloc(m->dyn_pi, (size_t)nc * sizeof(*m->dyn_pi));
        m->dyn_next = (int *)realloc(m->dyn_next, (size_t)nc * sizeof(*m->dyn_next));
        m->dyn_cap  = nc;
    }
    m->dyn_cfd[m->dyn_n]  = cfd;
    m->dyn_pi[m->dyn_n]   = pi;
    m->dyn_next[m->dyn_n] = m->dyn_head[sl];
    m->dyn_head[sl]       = m->dyn_n++;
}

static void eff_acc_add_callee(EffAcc *acc, const Binding *b) {
    if (!acc->callees) return;
    if (!b) { if (acc->callee_overflow) *acc->callee_overflow = true; return; }
    for (int k = 0; k < *acc->n_callees; k++)
        if (acc->callees[k] == b) return;
    if (*acc->n_callees < acc->cap_callees) acc->callees[(*acc->n_callees)++] = b;
    else if (acc->callee_overflow) *acc->callee_overflow = true;
}

/* Walk the *source* expression `e`, recording (a) the effects it performs and
 * handles, and (b) its direct callees -- both over the raw Expr (not the CTerm).
 * The raw walk is what makes co-classification sound: a `perform` / `handle`
 * buried under a form the CPS translator does not lower (e.g. `match`) is
 * invisible in the CTerm (it collapses to CT_UNSUPPORTED) but still emits a
 * *fiber* effect op, so it must taint the effect for any CPS peer.  It also
 * covers UNCOLORED performers (a function whose only control op is hidden from
 * coloring never becomes a CPS candidate, yet still fiber-performs), which the
 * CTerm view cannot see at all.  The callee walk backs the call-path taint: an
 * intermediary that neither performs nor handles E, but conducts a call from a
 * CPS handler of E down to a CPS performer of E, is still a DK-chain link.
 *
 * Effects are dynamically scoped -- a `perform` and its `handle` must run on the
 * same machine (both DK, or both fiber) -- so this set is what the fixpoint
 * compares across every top-level function. */

/* serial-receiver-effect-under-if-closure-or-leaf (shape 2): may an effect
 * escape this closure receiver's body?  The same test cps_ir.c's
 * fn_effect_may_escape applies to a named receiver: its declared row, else
 * its inferred one, is not runtime-pure. */
static bool serial_closure_recv_escapes(const FnDef *fd) {
    if (!fd) return false;
    if (fd->binding && fd->binding->type.kind == TY_FN
        && !effect_row_is_runtime_pure(fd->binding->type.as.fn.effect_row))
        return true;
    if (!fd->inferred_effect_row) return true;
    return !effect_row_is_runtime_pure(fd->inferred_effect_row);
}
static const Expr *peel_fn_value(const Expr *e);

static void expr_collect_effects_acc(const Expr *e, EffAcc *acc) {
    if (!e) return;
    #define REC(x) expr_collect_effects_acc((x), acc)
    switch (e->kind) {
        /* --- the effect operations themselves --- */
        case EX_PERFORM:
            if (e->as.perform_.perform) {
                mark_effect(e->as.perform_.perform->effect_name, acc->plo, acc->phi);
                for (uint32_t i = 0; i < e->as.perform_.perform->n_args; i++)
                    REC(e->as.perform_.perform->args[i]);
            }
            return;
        case EX_HANDLE:
            if (e->as.handle_.handle) {
                HandleExpr *h = e->as.handle_.handle;
                REC(h->body);
                for (uint8_t i = 0; i < h->n_cases; i++) {
                    mark_effect(h->cases[i].effect_name, acc->hlo, acc->hhi);
                    REC(h->cases[i].body);
                }
            }
            return;
        case EX_HANDLER_LIT:
            /* B3 part 2: a bare `(handler ...)` VALUE construction does NOT handle
             * its effects here -- only the with-handler / handle APPLICATION does
             * (EX_WITH_HANDLER marks the discharge from the handler type row).
             * Marking the handled effect at the construction site would wrongly
             * attribute it to the constructing fn (e.g. a `main` that builds a
             * handler value and passes it on), permanently tainting the effect.
             * Still recurse into the case bodies for any OTHER effect they perform. */
            if (e->as.handle_.handle) {
                HandleExpr *h = e->as.handle_.handle;
                for (uint8_t i = 0; i < h->n_cases; i++) REC(h->cases[i].body);
            }
            return;
        case EX_RESUME:
            if (e->as.resume_.resume) { REC(e->as.resume_.resume->k); REC(e->as.resume_.resume->value); }
            return;
        case EX_DISCONTINUE:
            if (e->as.discontinue_.discontinue) {
                REC(e->as.discontinue_.discontinue->k);
                REC(e->as.discontinue_.discontinue->exception);
            }
            return;
        case EX_WITH_HANDLER: {
            /* B3 part 2: mark the effects this handler value DISCHARGES (from its
             * type's handled_row) into the HAND set.  Recursing into a `(handler
             * ...)` LITERAL already marks its inline cases, but a DYNAMIC handler
             * value (a parameter / `(.field obj)` read) has no inline cases, so
             * without this a fn that discharges an effect via a handler parameter
             * (e.g. run-with) is wrongly seen as leaving the effect unhandled and
             * co-taints to the fiber.  The type row covers both. */
            const Type *ht = &e->as.with_handler_.handler->type;
            if (ht->kind == TY_HANDLER && ht->as.handler_.handled_row) {
                const Symbol *effs[32]; uint8_t neff = 0;
                effect_row_collect_names(ht->as.handler_.handled_row, effs, &neff, 32);
                for (uint8_t i = 0; i < neff; i++)
                    mark_effect(effs[i], acc->hlo, acc->hhi);
            }
            REC(e->as.with_handler_.handler); REC(e->as.with_handler_.body); return;
        }
        case EX_COMPOSE_HANDLERS: REC(e->as.compose_handlers_.h1); REC(e->as.compose_handlers_.h2); return;
        /* --- structural / binding forms: recurse into every sub-expression --- */
        case EX_LET:
        case EX_LETREC: { /* reuses the let_ union member */
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                Expr *init = e->as.let_.bindings[i].init;
                const Binding *lb = e->as.let_.bindings[i].binding;
                /* A closure literal bound to a let/hoist temp that RECORDS it
                 * (closure_fn_binding) is not an independent value-use of the
                 * lifted lambda -- the temp's own uses are counted through the
                 * EX_VAR indirection.  Still recurse into the closure body so
                 * its effects collect.  Without this, the hoisted-callback
                 * shape reads uses=2 ok=1 and is never threadable. */
                const Binding *lb_lam = lb
                    ? (lb->closure_fn_binding ? lb->closure_fn_binding
                                              : lb->hoist_closure_fn_binding)
                    : NULL;
                if (init && init->kind == EX_CLOSURE && lb_lam &&
                    init->as.closure_.closure &&
                    init->as.closure_.closure->fn &&
                    init->as.closure_.closure->fn->binding == lb_lam) {
                    REC(init->as.closure_.closure->fn->body);
                } else {
                    REC(init);
                }
            }
            REC(e->as.let_.body); return;
        }
        case EX_IF:    REC(e->as.if_.cond); REC(e->as.if_.then_); REC(e->as.if_.else_or_null); return;
        case EX_DO:    for (uint32_t i = 0; i < e->as.do_.n; i++) REC(e->as.do_.items[i]); return;
        case EX_WHILE: REC(e->as.while_.cond); REC(e->as.while_.body); return;
        case EX_SET:   REC(e->as.set_.value); return;
        case EX_DEF:   REC(e->as.def_.init); return;
        case EX_BUILTIN: for (uint32_t i = 0; i < e->as.builtin.n; i++) REC(e->as.builtin.args[i]); return;
        case EX_CALL:
            /* Direct call -> record the callee; indirect call -> overflow (may
             * reach any colored peer).  fn_expr is NULL for a resolved direct
             * call, non-NULL only for the indirect/higher-order case. */
            if (e->as.call_.fn_binding) eff_acc_add_callee(acc, e->as.call_.fn_binding);
            else if (e->as.call_.fn_expr) {
                /* Effect subtyping / capability field (docs/reported/
                 * cps-effect-subtype-capability-pure-fn-in-effectful-field.md):
                 * a call THROUGH a lowered `.field` capability access (fn_expr is
                 * an EX_GET_FIELD carrying an adt_ctor) is credited with the
                 * FIELD's PRECISE effect row -- mirroring effect_check's
                 * collect_effects_in_expr field path -- rather than the blunt
                 * "reaches every colored peer" overflow.  So a handled body whose
                 * only interior call is a capability invocation whose inferred row
                 * is empty (a pure value stored in an effectful-typed field, which
                 * the compiler already proves -- it raises TUR-W0033) is NOT
                 * force-evicted.  A genuinely effectful field VALUE is still caught
                 * globally: its stored fn is address-taken -> a permanent fiber
                 * source -> the effect base-taints and the handler co-evicts
                 * (SIG-TAINT), independent of this local crediting.  Gated on
                 * cps-tramp-resume so the shipping classifier stays byte-identical
                 * (flag-off keeps the unconditional overflow). */
                const struct CtorDef *fac = NULL; uint32_t ffidx = 0;
                if (e->as.call_.fn_expr->kind == EX_GET_FIELD) {
                    fac   = e->as.call_.fn_expr->as.get_field_.adt_ctor;
                    ffidx = e->as.call_.fn_expr->as.get_field_.field_idx;
                }
                if (fac) {
                    const struct EffectRow *fr = (ffidx < fac->n_fields)
                        ? fac->fields[ffidx].effect_row : NULL;
                    if (fr && fr->kind == ERK_CONCRETE)
                        for (uint8_t i = 0; i < fr->as.concrete.n_effects; i++)
                            if (fr->as.concrete.effects[i])
                                mark_effect(fr->as.concrete.effects[i]->name, acc->plo, acc->phi);
                } else if (acc->callees && acc->callee_overflow) {
                    *acc->callee_overflow = true;
                }
            }
            if (acc->callee_target && e->as.call_.fn_binding == acc->callee_target
                && acc->callee_out)
                (*acc->callee_out)++;
            /* E2/taint-completeness (cps-tramp-resume): a call THROUGH a fn-value
             * performs the fn-value's DECLARED effect row.  Credit its concrete
             * effects as performs (whether the callee is a fn-typed param binding or
             * an fn_expr) so an effect reached ONLY through a fn-value taints -- a
             * fn evicted for that call then seeds it and its handler co-evicts.
             * Unconditional since cps-tramp-resume graduated (2026-07-19). */
            const Type *ft = NULL;
            const Binding *fb = e->as.call_.fn_binding;
            if (fb && !fb->is_global && fb->type.kind == TY_FN) ft = &fb->type;
            else if (!fb && e->as.call_.fn_expr && e->as.call_.fn_expr->type.kind == TY_FN)
                ft = &e->as.call_.fn_expr->type;
            const struct EffectRow *row = ft ? ft->as.fn.effect_row : NULL;
            if (row && row->kind == ERK_CONCRETE)
                for (uint8_t i = 0; i < row->as.concrete.n_effects; i++)
                    if (row->as.concrete.effects[i])
                        mark_effect(row->as.concrete.effects[i]->name, acc->plo, acc->phi);
            /* E2 threadability probe: is count_target passed here as arg[i] to a
             * THREADING param of a resolvable global callee?  Counted alongside the
             * total-use walk below, so the caller can test total == threadable. */
            if (acc->thr_ok && acc->count_target && acc->thr_program) {
                const FnDef *cfd = e->as.call_.fn_binding
                    ? fd_for_binding(acc->thr_program, e->as.call_.fn_binding) : NULL;
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    const Expr *a = peel_fn_value(e->as.call_.args[i]);
                    bool is_target = a && a->kind == EX_VAR
                        && a->as.var.binding == acc->count_target;
                    /* fn-value-fat-normalization (effect-row increment): a
                     * CAPTURING closure literal is a use of its lifted FnDef's
                     * binding in the same threadable-arg sense -- under
                     * normalization the fat box's slot 0 is that entry, and
                     * the E2a dispatch threads it when registered.  A let /
                     * `^borrow`-hoist temp of such a closure records the lifted
                     * lambda in closure_fn_binding; follow it. */
                    if (!is_target && a && a->kind == EX_CLOSURE
                        && a->as.closure_.closure && a->as.closure_.closure->fn
                        && a->as.closure_.closure->fn->binding == acc->count_target)
                        is_target = true;
                    if (!is_target && a && a->kind == EX_VAR && a->as.var.binding
                        && (a->as.var.binding->closure_fn_binding == acc->count_target
                            || a->as.var.binding->hoist_closure_fn_binding == acc->count_target))
                        is_target = true;
                    if (is_target && cfd) {
                        PtClass cls = param_thread_class(cfd, i);
                        if (cls != PT_NONE) {
                            (*acc->thr_ok)++;
                            if (acc->thr_tier && (int)cls > *acc->thr_tier)
                                *acc->thr_tier = (int)cls;
                        }
                    }
                }
            }
            if (acc->multi && acc->thr_program) {
                FvMulti *m = (FvMulti *)acc->multi;
                const FnDef *cfd = NULL;
                bool cfd_done = false;
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    int sls[3];
                    int ns = fvm_ref_slots(m, peel_fn_value(e->as.call_.args[i]), sls);
                    if (!ns) continue;
                    if (!cfd_done) {
                        cfd = e->as.call_.fn_binding
                            ? fd_for_binding(acc->thr_program, e->as.call_.fn_binding) : NULL;
                        cfd_done = true;
                    }
                    if (!cfd) break;
                    PtClass cls = param_thread_class(cfd, i);
                    if (cls == PT_NONE) continue;
                    for (int k = 0; k < ns; k++) {
                        m->ok[sls[k]]++;
                        if (cfd->closure) fvm_dyn_add(m, sls[k], cfd, i);
                        else if ((int)cls > m->tier[sls[k]]) m->tier[sls[k]] = (int)cls;
                    }
                }
            }
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) REC(e->as.call_.args[i]);
            REC(e->as.call_.fn_expr); REC(e->as.call_.dict_arg); return;
        case EX_RETURN: REC(e->as.return_.value); return;
        case EX_MATCH:
            REC(e->as.match_.scrutinee);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                REC(e->as.match_.arms[i].guard);
                REC(e->as.match_.arms[i].body);
            }
            return;
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++) REC(e->as.make_struct_.field_values[i]);
            return;
        case EX_GET_FIELD: REC(e->as.get_field_.struct_expr); return;
        case EX_SET_FIELD: REC(e->as.set_field_.receiver); REC(e->as.set_field_.value); return;
        case EX_SET_DEREF: REC(e->as.set_deref_.ref); REC(e->as.set_deref_.value); return;
        /* delimited-control */
        case EX_RESET:            REC(e->as.reset_.body); return;
        case EX_SHIFT:            REC(e->as.shift_.k_fn); REC(e->as.shift_.body); return;
        case EX_SHIFT0:           REC(e->as.shift0_.k_fn); REC(e->as.shift0_.body); return;
        case EX_CALLCC:           REC(e->as.callcc_.fn); return;
        case EX_CLONEABLE_RESET:  REC(e->as.cloneable_reset_.body); return;
        case EX_CLONEABLE_SHIFT:  REC(e->as.cloneable_shift_.k_fn); REC(e->as.cloneable_shift_.body); return;
        case EX_SERIAL_RESET:     REC(e->as.serial_reset_.body); return;
        case EX_SERIAL_SHIFT: {
            /* serial-receiver-effect-under-if-closure-or-leaf (shape 2): a
             * CAPTURING closure receiver an effect escapes is called OUTWARD,
             * through its env-taking `__cps` twin (emit_serial_outward_call), so
             * the reset's own continuation -- and the handlers above it -- is
             * its downstream chain.  That twin exists only for a lambda in the
             * threadable set, and this use is exactly a threading one: the call
             * is a tail call on the reset's continuation (tier `now`). */
            const Expr *kf = peel_fn_value(e->as.serial_shift_.k_fn);
            if (acc->multi && kf && kf->kind == EX_CLOSURE
                && kf->as.closure_.closure && kf->as.closure_.closure->fn
                && serial_closure_recv_escapes(kf->as.closure_.closure->fn)) {
                int sl = fvm_slot(acc->multi, kf->as.closure_.closure->fn->binding);
                if (sl >= 0) {
                    acc->multi->ok[sl]++;
                    if ((int)PT_NOW > acc->multi->tier[sl])
                        acc->multi->tier[sl] = (int)PT_NOW;
                }
            }
            REC(e->as.serial_shift_.k_fn); REC(e->as.serial_shift_.body); return;
        }
        /* async / STM */
        case EX_ASYNC:      REC(e->as.async_.fn_expr); return;
        case EX_AWAIT:      REC(e->as.await_.fut_expr); return;
        case EX_ATOMICALLY: REC(e->as.atomically_.stm_expr); return;
        case EX_CHECK:      REC(e->as.check_.cond); return;
        case EX_OR_ELSE:    REC(e->as.or_else_.stm1); REC(e->as.or_else_.stm2); return;
        case EX_STM:        for (uint32_t i = 0; i < e->as.stm_.n_body; i++) REC(e->as.stm_.body[i]); return;
        case EX_TVAR_NEW:    REC(e->as.tvar_new_.init); return;
        case EX_TVAR_READ:   REC(e->as.tvar_read_.tvar); return;
        case EX_TVAR_WRITE:  REC(e->as.tvar_write_.tvar); REC(e->as.tvar_write_.value); return;
        case EX_TVAR_MODIFY: REC(e->as.tvar_modify_.tvar); REC(e->as.tvar_modify_.fn); return;
        case EX_TVAR_SWAP:   REC(e->as.tvar_swap_.tvar); REC(e->as.tvar_swap_.new_val); return;
        case EX_TVAR_CAS:    REC(e->as.tvar_cas_.tvar); REC(e->as.tvar_cas_.old_val); REC(e->as.tvar_cas_.new_val); return;
        /* single-operand wrappers */
        case EX_REF:          REC(e->as.ref_.expr); return;
        case EX_DEREF:        REC(e->as.deref_.expr); return;
        case EX_BORROW_IMMUT: REC(e->as.borrow_immut_.expr); return;
        case EX_BORROW_MUT:   REC(e->as.borrow_mut_.expr); return;
        case EX_ASCRIBE:      REC(e->as.ascribe_.inner); return;
        case EX_REINTERPRET:  REC(e->as.reinterpret_.expr); return;
        case EX_CAST:         REC(e->as.cast_.expr); return;
        case EX_FN_TO_FAT:    REC(e->as.fn_to_fat_.inner); return;
        case EX_POLY_TO_FAT:  REC(e->as.poly_to_fat_.inner); return;
        case EX_POLY_WRAP:    REC(e->as.poly_wrap_.inner); return;
        case EX_CONT_PRED:    REC(e->as.cont_pred_.expr); return;
        case EX_RC_OF:        REC(e->as.rc_of_.expr); return;
        case EX_RC_CLONE:     REC(e->as.rc_clone_.expr); return;
        case EX_RC_DROP:      REC(e->as.rc_drop_.expr); return;
        case EX_RC_PTR:       REC(e->as.rc_ptr_.expr); return;
        case EX_RC_COUNT:     REC(e->as.rc_count_.expr); return;
        case EX_RC_FROM_REF:  REC(e->as.rc_from_ref_.expr); return;
        case EX_REF_FROM_RC:  REC(e->as.ref_from_rc_.expr); return;
        case EX_WEAK:         REC(e->as.weak_.expr); return;
        case EX_WEAK_UPGRADE: REC(e->as.weak_upgrade_.expr); return;
        case EX_WEAK_PRED:    REC(e->as.weak_pred_.expr); return;
        case EX_REF_PRED:     REC(e->as.ref_pred_.expr); return;
        case EX_PANIC:        REC(e->as.panic_.payload); return;
        case EX_PANIC_WITH:   REC(e->as.panic_with_.payload); return;
        case EX_CATCH_UNWIND: REC(e->as.catch_unwind_.thunk); return;
        case EX_CATCH_PANIC_OF: REC(e->as.catch_panic_of_.thunk); return;
        case EX_UNION_INJECT: REC(e->as.union_inject_.value); return;
        case EX_ANY_TYPE_OF:  REC(e->as.any_type_of_.value); return;
        case EX_ANY_IS:       REC(e->as.any_is_.value); return;
        case EX_EXISTS_PACK:  REC(e->as.exists_pack_.value); return;
        case EX_EXISTS_OPEN:  REC(e->as.exists_open_.packed); REC(e->as.exists_open_.body); return;
        case EX_EXISTS_DISPATCH:
            for (uint32_t i = 0; i < e->as.exists_dispatch_.n_args; i++) REC(e->as.exists_dispatch_.args[i]);
            return;
        case EX_DEFER: REC(e->as.defer_.body); return;
        /* generators, dynvars, list/set literals, checked cast, cont-app */
        case EX_ANY_CAST:  REC(e->as.any_cast_.value); return;
        case EX_CONS_LIST: for (uint32_t i = 0; i < e->as.cons_list_.n; i++) REC(e->as.cons_list_.items[i]); return;
        case EX_SET_LIT:   for (uint32_t i = 0; i < e->as.set_lit_.n; i++) REC(e->as.set_lit_.items[i]); return;
        case EX_YIELD:     REC(e->as.yield_.value); return;
        case EX_GEN:       if (e->as.gen_.def) REC(e->as.gen_.def->body); return;
        case EX_GEN_NEXT:  REC(e->as.gen_next_.gen_expr); return;
        case EX_GEN_UNWRAP: REC(e->as.gen_unwrap_.ptr_expr); return;
        case EX_GEN_DONE:  REC(e->as.gen_done_.gen_expr); return;
        case EX_DEFDYNAMIC:   REC(e->as.defdynamic_.root_expr); return;
        case EX_DYNVAR_SET:   REC(e->as.dynvar_set_.value); return;
        case EX_DYNVAR_BINDING:
            for (uint32_t i = 0; i < e->as.dynvar_binding_.n_pairs; i++)
                REC(e->as.dynvar_binding_.pairs[i].override_expr);
            REC(e->as.dynvar_binding_.body); return;
        case EX_CPS_CONT_APP: REC(e->as.cps_cont_app_.cont); REC(e->as.cps_cont_app_.value); return;
        case EX_PANIC_PAYLOAD_TYPE:  REC(e->as.panic_payload_type_.payload); return;
        case EX_PANIC_PAYLOAD_VALUE: REC(e->as.panic_payload_value_.payload); return;
        case EX_PANIC_PAYLOAD_FILE:  REC(e->as.panic_payload_file_.payload); return;
        case EX_PANIC_PAYLOAD_LINE:  REC(e->as.panic_payload_line_.payload); return;
        case EX_PANIC_PAYLOAD_DOWNS: REC(e->as.panic_payload_downs_.payload); return;
        /* nested function / closure: descend into its body so a lambda that
         * performs an effect taints the effect for its enclosing function too. */
        case EX_CLOSURE:
            /* fn-value-fat-normalization (effect-row increment): the closure
             * LITERAL is a value-use of its lifted FnDef's binding -- count it
             * so fn_value_threadable's total matches the threadable-arg count
             * (a capturing callback into an effectful slot used to read as
             * uses=0 and could never be threadable). */
            if (acc->count_target && acc->count_out
                && e->as.closure_.closure && e->as.closure_.closure->fn
                && e->as.closure_.closure->fn->binding == acc->count_target)
                (*acc->count_out)++;
            if (acc->multi && e->as.closure_.closure && e->as.closure_.closure->fn) {
                int sl = fvm_slot(acc->multi, e->as.closure_.closure->fn->binding);
                if (sl >= 0) acc->multi->total[sl]++;
            }
            if (e->as.closure_.closure && e->as.closure_.closure->fn)
                REC(e->as.closure_.closure->fn->body);
            return;
        case EX_FN_DEF: if (e->as.fn_def_.fn) REC(e->as.fn_def_.fn->body); return;
        case EX_FN:     if (e->as.fn_.fn)     REC(e->as.fn_.fn->body);     return;
        /* A fn-VALUE USE: referencing a top-level (colored) fn as a value -- to
         * pass it as a higher-order argument, store it, or return it -- means that
         * fn may be CALLED downstream through the value.  Record it as a reachable
         * callee so the call-path taint (ensure_S Rule C) spans the DATA FLOW: a
         * fiber intermediary that hands a colored performer to a DK conduit (e.g.
         * `apply-logged` passing a Log-performing lifted closure to `apply`) sits
         * on the handler->performer path and must taint the effect, or the DK
         * performer runs with the intermediary having severed the handler's DK
         * chain (`unhandled effect`).  Precise -- adds only the SPECIFIC fn, not the
         * `edges_all` over-approximation that evicts every higher-order caller. */
        case EX_VAR:
            if (acc->count_target && e->as.var.binding == acc->count_target
                && acc->count_out)
                (*acc->count_out)++;
            /* A let/hoist temp of a capturing closure counts as a use of the
             * lifted lambda it records (closure_fn_binding) -- pairs with the
             * threadable-arg probe's identical indirection above. */
            else if (acc->count_target && acc->count_out && e->as.var.binding
                     && (e->as.var.binding->closure_fn_binding == acc->count_target
                         || e->as.var.binding->hoist_closure_fn_binding == acc->count_target))
                (*acc->count_out)++;
            if (acc->multi) {
                int sls[3];
                int ns = fvm_ref_slots(acc->multi, e, sls);
                for (int k = 0; k < ns; k++) acc->multi->total[sls[k]]++;
            }
            if (e->as.var.binding && e->as.var.binding->type.kind == TY_FN) {
                if (!acc->calls_only) eff_acc_add_callee(acc, e->as.var.binding);
                /* E2/taint-completeness (cps-tramp-resume): a fn-value reference in
                 * VALUE position (this EX_VAR is not a direct-call callee -- those
                 * carry fn_binding with fn_expr=NULL and never reach here) means the
                 * fn's ADDRESS is taken; its DK direct-entry, invoked indirectly,
                 * escapes a perform.  Record the global fn as address-taken so an
                 * effectful one is forced to fiber (see ensure_S). */
                if (g_addr_collecting && e->as.var.binding->is_global)
                    addr_taken_add(e->as.var.binding);
            }
            return;
        case EX_SELECT:
            for (uint32_t i = 0; i < e->as.select_.n_clauses; i++) {
                REC(e->as.select_.clauses[i].chan);
                REC(e->as.select_.clauses[i].send_val);
                REC(e->as.select_.clauses[i].body);
            }
            REC(e->as.select_.default_body);
            return;
        default:
            /* Leaves (literals, var, dict, extern-c/inline-c, type/def decls) and
             * panic-payload accessors carry no effect operation. */
            return;
    }
    #undef REC
}

/* Combined "performs-or-handles" effect set (no callee collection) -- the shape
 * every existing caller uses.  Perform and handle tags fold into the same set. */
static void expr_collect_effects(const Expr *e, uint64_t *lo, uint64_t *hi) {
    EffAcc acc = { lo, hi, lo, hi, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL,
                   NULL, NULL, NULL, false, NULL };
    expr_collect_effects_acc(e, &acc);
}

/* B5 (cps-tramp-resume): the NET escaping effect set of `e` -- the effects
 * PERFORMED in `e` that are NOT discharged by a `handle` enclosing the perform
 * WITHIN `e`.  A self-handling body -- `(fn [] (handle (perform E) (E [x] k)
 * (resume k ...)))`, the async-closure shape in `effects-async` -- performs E
 * but handles it in the same body, so E does not escape when the fn is invoked
 * indirectly (its interior handle installs its own DK prompt).  Structural, so a
 * perform sitting OUTSIDE a same-effect handle still escapes -- unlike a blunt
 * performed-minus-handled set-subtract (which would wrongly clear
 * `(do (perform E) (handle pure (E ...)))`).  A handler CASE body's own performs
 * DO escape (they run in the handler frame, above this prompt), so they are
 * counted.  An uncovered form falls back to the raw union (conservative: it
 * over-counts handled effects as escaping, never under-counts). */
static void fn_net_escaping_acc(const Expr *e, uint64_t *lo, uint64_t *hi) {
    if (!e) return;
    #define NESC(x) fn_net_escaping_acc((x), lo, hi)
    switch (e->kind) {
        case EX_PERFORM:
            if (e->as.perform_.perform) {
                mark_effect(e->as.perform_.perform->effect_name, lo, hi);
                for (uint32_t i = 0; i < e->as.perform_.perform->n_args; i++)
                    NESC(e->as.perform_.perform->args[i]);
            }
            return;
        case EX_HANDLE:
            if (e->as.handle_.handle) {
                HandleExpr *h = e->as.handle_.handle;
                /* the body's net-escaping effects, MINUS the effects this handle
                 * discharges */
                uint64_t blo = 0, bhi = 0;
                fn_net_escaping_acc((const Expr *)h->body, &blo, &bhi);
                uint64_t hlo = 0, hhi = 0;
                for (uint8_t i = 0; i < h->n_cases; i++)
                    mark_effect(h->cases[i].effect_name, &hlo, &hhi);
                *lo |= (blo & ~hlo);
                *hi |= (bhi & ~hhi);
                /* case bodies run in the handler frame -- their performs escape */
                for (uint8_t i = 0; i < h->n_cases; i++)
                    NESC(h->cases[i].body);
            }
            return;
        case EX_REINTERPRET: NESC(e->as.reinterpret_.expr); return;
        case EX_ASCRIBE: NESC(e->as.ascribe_.inner); return;
        case EX_RETURN:  NESC(e->as.return_.value);  return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) NESC(e->as.do_.items[i]);
            return;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++) NESC(e->as.let_.bindings[i].init);
            NESC(e->as.let_.body);
            return;
        case EX_IF:
            NESC(e->as.if_.cond); NESC(e->as.if_.then_); NESC(e->as.if_.else_or_null);
            return;
        case EX_MATCH:
            NESC(e->as.match_.scrutinee);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                NESC(e->as.match_.arms[i].guard);
                NESC(e->as.match_.arms[i].body);
            }
            return;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) NESC(e->as.builtin.args[i]);
            return;
        case EX_CALL:
            NESC(e->as.call_.fn_expr);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) NESC(e->as.call_.args[i]);
            NESC(e->as.call_.dict_arg);
            return;
        default:
            /* Uncovered form: fall back to the raw union (perform+handle folded),
             * which over-counts a nested handled effect as escaping -- sound (it
             * never clears a genuinely-escaping perform), just conservative. */
            expr_collect_effects(e, lo, hi);
            return;
    }
    #undef NESC
}

/* A CT_LETRAW (delegated direct-emitted op) that performs NO fiber effect: its
 * delegated expr `e` contains no direct perform/handle (even one buried in a
 * delegated match/do/let, which the raw walk surfaces), and every callee it
 * invokes is provably effect-free (callee_effect_free -- a declared-empty row is
 * the sound transitive summary: an effect-free callee performs nothing, directly
 * or transitively).  An indirect / over-cap callee (callee_overflow -> `ov`) is
 * conservatively NOT effect-free.  Used by handle_delim_ok to admit a pure
 * delegated op inside a handled body's delimited position -- it cannot leave an
 * effect unhandled by the handle's DK prompt, so the subsequent KK_PROMPT
 * delivery stays the handle's own result. */
static bool letraw_effect_free(const CTerm *t) {
    if (!t || t->kind != CT_LETRAW || !t->as.letraw.e) return false;
    uint64_t lo = 0, hi = 0; bool ov = false;
    const Binding *callees[64]; int nc = 0;
    /* calls_only: a raw op that merely REFERENCES an effectful fn -- boxing
     * `my-eff` into a fat closure to pass it as a `^fat` argument -- runs no
     * effect; the effect happens at the eventual call, which is a separate
     * CTerm node with its own admission.  Counting the reference as a callee
     * here rejected the box construction, which dropped the whole handled body
     * to term_core_ok, whose CT_APPCONT case refuses the KK_PROMPT delivery a
     * `(handle (do (f g) 0) ...)` ends with.  That evicted the handler fn,
     * tainted its effect, co-evicted the performer, and landed its `perform` in
     * the direct emitter -- an ICE.  See
     * docs/archive/history/named-effectful-defn-as-fat-fn-value-ices.md.  A genuine
     * delegated CALL to an effectful callee is still rejected: that is the case
     * this gate exists for (its effect would run on the fiber, escaping the
     * handle's DK prompt). */
    EffAcc acc = { &lo, &hi, &lo, &hi, callees, &nc, 64, &ov,
                   NULL, NULL, NULL, NULL, NULL, NULL, NULL, true, NULL };
    expr_collect_effects_acc(t->as.letraw.e, &acc);
    if (lo || hi || ov) return false;
    for (int i = 0; i < nc; i++)
        if (!callee_effect_free(callees[i])) return false;
    return true;
}

/* ---- E2 fn-value threadability analysis (cps-tramp-resume) ------------------
 *
 * A codegen-NEUTRAL coloring pass that answers: for an effectful fn-value (an
 * address-taken named fn or an effectful lifted lambda) currently EVICTED to the
 * fiber, could it instead thread the DK continuation?  Per the plan's coloring
 * invariant (docs/archive/cps-dk-sole-effect-lowering-plan.md, "the invariant
 * that makes E2 a COLORING problem"): a fn-value is DK-threadable ONLY IF *every*
 * one of its value-uses is a DK-threadable site.  If ANY use is un-threadable (an
 * inline-C fn-ptr cast, a fiber HOF, a non-threading argument position, a stored
 * dict slot), the fn-value must stay on the fiber -- else its direct entry installs
 * a fresh root at that site and a perform escapes.
 *
 * This pass computes that decision but does NOT yet consume it: g_threadable_fn is
 * populated and traced ([E2-COLOR]) so the concrete E2 target surface is measured
 * against the effect corpus BEFORE any ABI change.  Because nothing reads the set,
 * emission is byte-identical on both configs; the coloring is validated first, the
 * threading channel (E2a/b/c) lands on top of it.  The seed relation implemented
 * here: "every value-use is arg[i] to a THREADING param of a colored callee." */

/* Exhaustive count of ALL uses of `b` in `e`: every EX_VAR(b) reference (value
 * uses AND fn_expr-carried callees) PLUS every EX_CALL whose fn_binding is `b` (a
 * param called as `(b x)` carries b in fn_binding with no EX_VAR node).  Reuses the
 * audited effect-walk traversal so no form is missed -- an exhaustive count, which
 * is what makes the "all uses are tail-callee" test below sound.  A direct call to
 * a GLOBAL fn also lands in fn_binding, so this must only be used to count PARAM
 * uses (a param's fn_binding-call is genuinely a use); it is not used for global
 * fn-values (see fn_value_threadable, which leaves callee_target NULL). */
static int expr_count_all_uses(const Expr *e, const Binding *b) {
    if (!e || !b) return 0;
    int n = 0;
    uint64_t dl = 0, dh = 0;
    EffAcc acc = { &dl, &dh, &dl, &dh, NULL, NULL, 0, NULL, b, &n, NULL, NULL,
                   b, &n, NULL, false, NULL };
    expr_collect_effects_acc(e, &acc);
    return n;
}

/* Self-recursion context for ptc_walk: the fn whose param is being tiered and the
 * param's own position.  Set by param_thread_class before each ptc_walk so a
 * `(fd ... p ...)` recursive call passing `p` back at position `self_pi` is not
 * miscounted as an escape.  NULL/0 disables the exemption. */
static const Binding *g_ptc_self_bind;
static uint32_t       g_ptc_self_pi;

/* The `let` aliases of the param being tiered that are in scope at this point of
 * the walk (cps_ir_fnparam_alias_src).  A call through one is a call through the
 * param -- the translator inlines it as exactly that (pap_register_let) -- and any
 * other use of one is a use of the param, so every test below that asks "is this
 * `p`?" asks it of the aliases too.  Full set: further aliases are not followed
 * (counted as value-uses), which only ever declines. */
static const Binding *g_ptc_alias[8];
static uint32_t       g_ptc_n_alias;

static bool ptc_is_p(const Binding *b, const Binding *p) {
    if (!b) return false;
    if (b == p) return true;
    for (uint32_t i = 0; i < g_ptc_n_alias; i++)
        if (g_ptc_alias[i] == b) return true;
    return false;
}

/* expr_count_all_uses over `p` and its in-scope aliases. */
static int ptc_count_all_uses(const Expr *e, const Binding *p) {
    int n = expr_count_all_uses(e, p);
    for (uint32_t i = 0; i < g_ptc_n_alias; i++)
        n += expr_count_all_uses(e, g_ptc_alias[i]);
    return n;
}

/* Classify how param `p` of a HOF is used, tracking tail position: count its
 * value-uses (escapes -- passed as an arg, stored, bare ref), its tail-position
 * callee-calls, and its non-tail callee-calls.  A call to `p` under a `handle`/
 * `reset` body installed by the HOF itself is counted as a value-use: threading
 * `__kont` there would route the callee's perform past the HOF's OWN handler, so
 * it is not a clean thread.  Any occurrence of `p` inside a form this walk does not
 * structurally cover is absorbed (via the exhaustive counter) into value-uses --
 * a conservative default that keeps a miss from ever inflating threadability. */
static void ptc_walk(const Expr *e, const Binding *p, bool tail,
                     int *val, int *tailc, int *ntc) {
    if (!e) return;
    switch (e->kind) {
        case EX_CALL: {
            bool callee_is_p = ptc_is_p(e->as.call_.fn_binding, p)
                || (e->as.call_.fn_expr && e->as.call_.fn_expr->kind == EX_VAR
                    && ptc_is_p(e->as.call_.fn_expr->as.var.binding, p));
            if (callee_is_p) { if (tail) (*tailc)++; else (*ntc)++; }
            else ptc_walk(e->as.call_.fn_expr, p, false, val, tailc, ntc);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *a = peel_fn_value(e->as.call_.args[i]);
                if (a && a->kind == EX_VAR && ptc_is_p(a->as.var.binding, p)) {
                    /* E2 (cps-tramp-resume): a SELF-recursive call passing param `p`
                     * back at its OWN position is NOT an escape -- it is the same
                     * row-poly fn-value threading through the recursion, so it does
                     * not disqualify `p` as a thread-param (effect-poly-map).  Every
                     * other value-use (stored, passed elsewhere, bare ref) still
                     * counts -- an ALIAS passed back too: the translator inlines an
                     * alias only when its every use is a call. */
                    if (a->as.var.binding == p
                        && g_ptc_self_bind
                        && e->as.call_.fn_binding == g_ptc_self_bind
                        && i == g_ptc_self_pi)
                        continue;
                    (*val)++;
                }
                else ptc_walk(e->as.call_.args[i], p, false, val, tailc, ntc);
            }
            ptc_walk(e->as.call_.dict_arg, p, false, val, tailc, ntc);
            return;
        }
        case EX_VAR:  if (ptc_is_p(e->as.var.binding, p)) (*val)++; return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                ptc_walk(e->as.do_.items[i], p, tail && (i + 1 == e->as.do_.n),
                         val, tailc, ntc);
            return;
        case EX_LET: {
            /* `(let [f p] ...)` that the translator inlines (every use of `f` a
             * saturated call) renames `p`: its init is not a value-use, and the
             * calls through `f` below count as calls through `p`. */
            uint32_t saved = g_ptc_n_alias;
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (g_ptc_n_alias < 8 && cps_ir_let_fnparam_alias(e, i) == p) {
                    g_ptc_alias[g_ptc_n_alias++] = e->as.let_.bindings[i].binding;
                    continue;
                }
                ptc_walk(e->as.let_.bindings[i].init, p, false, val, tailc, ntc);
            }
            ptc_walk(e->as.let_.body, p, tail, val, tailc, ntc);
            g_ptc_n_alias = saved;
            return;
        }
        case EX_IF:
            ptc_walk(e->as.if_.cond, p, false, val, tailc, ntc);
            ptc_walk(e->as.if_.then_, p, tail, val, tailc, ntc);
            ptc_walk(e->as.if_.else_or_null, p, tail, val, tailc, ntc);
            return;
        case EX_ASCRIBE: ptc_walk(e->as.ascribe_.inner, p, tail, val, tailc, ntc); return;
        case EX_RETURN:  ptc_walk(e->as.return_.value, p, false, val, tailc, ntc); return;
        case EX_BUILTIN:  /* arithmetic / operator args are non-tail */
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                ptc_walk(e->as.builtin.args[i], p, false, val, tailc, ntc);
            return;
        case EX_WHILE:
            ptc_walk(e->as.while_.cond, p, false, val, tailc, ntc);
            ptc_walk(e->as.while_.body, p, false, val, tailc, ntc);
            return;
        case EX_SET:  ptc_walk(e->as.set_.value, p, false, val, tailc, ntc); return;
        case EX_DEF:  ptc_walk(e->as.def_.init, p, false, val, tailc, ntc); return;
        case EX_MAKE_STRUCT:  /* a fn-value stored in a field is a value-use (sink) */
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                ptc_walk(e->as.make_struct_.field_values[i], p, false, val, tailc, ntc);
            return;
        case EX_MATCH:
            ptc_walk(e->as.match_.scrutinee, p, false, val, tailc, ntc);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                ptc_walk(e->as.match_.arms[i].guard, p, false, val, tailc, ntc);
                ptc_walk(e->as.match_.arms[i].body, p, tail, val, tailc, ntc);
            }
            return;
        case EX_HANDLE: {
            /* E2 (cps-tramp-resume): a call to `p` in the handle BODY threads to
             * the handle's OWN installed prompt -- the delim's `cur_k`, which
             * carries this handler at its head -- NOT the bare outer `__kont`.  So
             * p's perform reaches this handler (matching effect) or walks past it
             * to an outer handler (non-matching) -- a clean thread either way,
             * exactly `(handle (f) (E ...) ...)` in `run-with`.  A TAIL-position
             * body call is therefore a threadable tail call, not a value-use.
             * Calls to `p` inside the handler CASE bodies run per-perform in the
             * handler's own frame (a distinct continuation); keep those as
             * value-uses.  An `unsafe`-marker handle is transparent (body emitted
             * in place, no real prompt), so threading its body's tail call to the
             * enclosing `__kont` is equally clean -- the same recursion covers it. */
            HandleExpr *h = e->as.handle_.handle;
            if (h) {
                ptc_walk((const Expr *)h->body, p, tail, val, tailc, ntc);
                for (uint32_t i = 0; i < h->n_cases; i++)
                    *val += ptc_count_all_uses(h->cases[i].body, p);
            }
            return;
        }
        default:
            /* Uncovered form (incl. EX_RESET, where a call to p runs under the
             * HOF's own prompt): treat every occurrence of p as a value-use so the
             * param cannot be judged threadable through it. */
            *val += ptc_count_all_uses(e, p);
            return;
    }
}

/* Tier the threadability of param `pi` of colored `fd` (see PtClass).  A param is
 * a threading candidate iff it NEVER escapes as a value -- every use is a call to
 * it.  The tier then records how hard that thread is: PT_NOW (tail-only, scalar
 * carrier), PT_NONTAIL (a non-tail call needs a threaded CT_LETCALL), or PT_E1
 * (an is_poly_fn / capturing param needs E1's carrier ABI first). */
static PtClass param_thread_class(const FnDef *fd, uint32_t pi) {
    if (!fd || !fd->cps_colored || !fd->body || !fd->params || pi >= fd->n_params)
        return PT_NONE;
    const Binding *p = fd->params[pi];
    if (!p) return PT_NONE;
    int val = 0, tc = 0, ntc = 0;
    g_ptc_self_bind = fd->binding; g_ptc_self_pi = pi;
    ptc_walk(fd->body, p, true, &val, &tc, &ntc);
    g_ptc_self_bind = NULL; g_ptc_self_pi = 0;
    if (val > 0 || (tc + ntc) < 1) return PT_NONE;   /* escapes, or never called */
    if (!fn_sig_ok(fd)) return PT_E1;                /* is_poly_fn / capturing param */
    /* E2a registry threading needs a CONCRETE effect row on the param (row-poly
     * params delegate to a fresh-root direct entry -> escape); tier those PT_E1. */
    /* E2a registry threading needs a CONCRETE effect row on the param.  A ROW-
     * VARIABLE param (`#fx{e}`) threads its callback's `__kont` just the same (the
     * DK thread is effect-agnostic), and the cross-HOF leaf-fiber delegation that
     * used to make this unsound is now skipped under the flag
     * (colored_call_wbd_delegatable), so admit row-variable params under
     * cps-tramp-resume.  Flag-off keeps the concrete-row requirement (codegen
     * unchanged). */
    if (ntc > 0) {
        /* E2c: a non-tail fn-value call inside a HOF that ALSO installs a `handle`
         * sits in the handle's LIFTED continuation frame.  That frame now CAPTURES
         * the via_registry callee as an int64 fn-ptr scalar (cap_add_fn_scalar +
         * the collect_caps/has_capture CT_TAILCALL cases), so the threaded
         * `__tur_cps_lookup(f)` reads `f` from the env instead of an out-of-scope
         * param.  A FAT (is_poly_fn) `f` fails the capture (cap_add_fn_scalar bails),
         * so collect_caps fails and needs_heap_join evicts it -- correct until the
         * fn_cps channel lands.  So a non-tail call always tiers PT_NONTAIL now. */
        return PT_NONTAIL;                            /* a non-tail fn-value call */
    }
    return PT_NOW;                                    /* tail-only, scalar carrier */
}


/* Peel the representation wrappers a fn-value acquires at a call site -- an
 * ascription, or the fat/poly boxing (EX_FN_TO_FAT / EX_POLY_TO_FAT) that a named
 * fn or captureless lambda gets when passed where a `(-> ...)` value is expected --
 * down to the underlying expression (usually the EX_VAR naming the fn). */
static const Expr *peel_fn_value(const Expr *e) {
    while (e) {
        switch (e->kind) {
            case EX_ASCRIBE:     e = e->as.ascribe_.inner;     break;
            case EX_FN_TO_FAT:   e = e->as.fn_to_fat_.inner;   break;
            case EX_POLY_TO_FAT: e = e->as.poly_to_fat_.inner; break;
            case EX_POLY_WRAP:   e = e->as.poly_wrap_.inner;   break;
            case EX_CAST:        e = e->as.cast_.expr;         break;
            case EX_REINTERPRET: e = e->as.reinterpret_.expr;  break;
            default:             return e;
        }
    }
    return e;
}

/* Resolve a global fn binding to its FnDef by scanning the program items. */
static const FnDef *fd_for_binding_scan(const Expr *program, const Binding *b) {
    if (!program || !b || program->kind != EX_PROGRAM) return NULL;
    uint32_t np = program->as.program.n;
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = program->as.program.items[i];
        if (!it) continue;
        if (it->kind == EX_FN_DEF && it->as.fn_def_.fn
            && it->as.fn_def_.fn->binding == b)
            return it->as.fn_def_.fn;
        /* A module member's FnDef lives in the EX_DEFMODULE body, not as a
         * top-level item -- descend so a colored module member resolves. */
        if (it->kind == EX_DEFMODULE && it->as.defmodule_.mod) {
            DefModule *m = it->as.defmodule_.mod;
            for (uint32_t j = 0; j < m->n_body; j++) {
                Expr *mb = m->body[j];
                if (mb && mb->kind == EX_FN_DEF && mb->as.fn_def_.fn
                    && mb->as.fn_def_.fn->binding == b)
                    return mb->as.fn_def_.fn;
            }
        }
    }
    return NULL;
}

/* r7rs-lang-plan R10: the same answer through a Binding* -> FnDef* table.
 * The scan above is linear in the program, and the effect walk asks it once
 * per call node of every function it colors -- quadratic in program size.
 * chibi's R7RS suite as one program (~1100 top-level forms over the stdlib)
 * spent over four minutes of `emit-c` here.  The table is rebuilt whenever
 * the program or its item count changes; first definition wins, as in the
 * scan. */
static const Expr     *fdc_prog;
static uint32_t        fdc_np, fdc_cap;
static const Binding **fdc_keys;
static const FnDef   **fdc_vals;
static uint32_t fdc_slot(const Binding *b) {
    uintptr_t h = (uintptr_t)b;
    h ^= h >> 17; h *= (uintptr_t)0x9E3779B97F4A7C15ull; h ^= h >> 29;
    return (uint32_t)h & (fdc_cap - 1);
}
static void fdc_put(const Binding *b, const FnDef *fd) {
    uint32_t k = fdc_slot(b);
    while (fdc_keys[k] && fdc_keys[k] != b) k = (k + 1) & (fdc_cap - 1);
    if (!fdc_keys[k]) { fdc_keys[k] = b; fdc_vals[k] = fd; }
}
static void fdc_build(const Expr *program) {
    uint32_t np = program->as.program.n, n = 0;
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = program->as.program.items[i];
        if (!it) continue;
        if (it->kind == EX_FN_DEF) n++;
        else if (it->kind == EX_DEFMODULE && it->as.defmodule_.mod) n += it->as.defmodule_.mod->n_body;
    }
    uint32_t cap = 16;
    while (cap < 2 * n + 2) cap <<= 1;
    free((void *)fdc_keys); free((void *)fdc_vals);
    fdc_keys = (const Binding **)calloc(cap, sizeof *fdc_keys);
    fdc_vals = (const FnDef **)calloc(cap, sizeof *fdc_vals);
    fdc_cap = cap;
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = program->as.program.items[i];
        if (!it) continue;
        if (it->kind == EX_FN_DEF && it->as.fn_def_.fn && it->as.fn_def_.fn->binding)
            fdc_put(it->as.fn_def_.fn->binding, it->as.fn_def_.fn);
        if (it->kind == EX_DEFMODULE && it->as.defmodule_.mod) {
            DefModule *m = it->as.defmodule_.mod;
            for (uint32_t j = 0; j < m->n_body; j++) {
                Expr *mb = m->body[j];
                if (mb && mb->kind == EX_FN_DEF && mb->as.fn_def_.fn && mb->as.fn_def_.fn->binding)
                    fdc_put(mb->as.fn_def_.fn->binding, mb->as.fn_def_.fn);
            }
        }
    }
    fdc_prog = program;
    fdc_np = np;
}
static const FnDef *fd_for_binding(const Expr *program, const Binding *b) {
    if (!program || !b || program->kind != EX_PROGRAM) return NULL;
    if (program != fdc_prog || program->as.program.n != fdc_np || !fdc_keys) fdc_build(program);
    if (!fdc_keys) return fd_for_binding_scan(program, b);
    uint32_t k = fdc_slot(b);
    while (fdc_keys[k]) {
        if (fdc_keys[k] == b) return fdc_vals[k];
        k = (k + 1) & (fdc_cap - 1);
    }
    return NULL;
}

/* The DK-threadable fn-value set: effectful address-taken fns / lifted lambdas
 * whose every value-use is a threadable argument.  Computed for measurement only
 * (traced [E2-COLOR]); not yet consumed by classification. */
static const Binding *g_threadable_fn[1024];
static int            g_threadable_fn_n;
static void threadable_add(const Binding *b) {
    for (int i = 0; i < g_threadable_fn_n; i++) if (g_threadable_fn[i] == b) return;
    if (g_threadable_fn_n < (int)(sizeof(g_threadable_fn)/sizeof(g_threadable_fn[0])))
        g_threadable_fn[g_threadable_fn_n++] = b;
}
static bool threadable_has(const Binding *b) {
    if (!b) return false;
    for (int i = 0; i < g_threadable_fn_n; i++) if (g_threadable_fn[i] == b) return true;
    return false;
}
static void threadable_remove(const Binding *b) {
    for (int i = 0; i < g_threadable_fn_n; i++)
        if (g_threadable_fn[i] == b) {
            g_threadable_fn[i] = g_threadable_fn[--g_threadable_fn_n];
            return;
        }
}

/* The fn-value binding an argument expression passes, if it is one: a named
 * fn or lifted lambda (EX_VAR), a lambda or closure literal (its FnDef), or a
 * let / hoist temp of a capturing closure.  Mirrors param_is_thread_safe. */
static const Binding *arg_fnval_binding(const Expr *arg) {
    const Expr *a = peel_fn_value(arg);
    if (!a) return NULL;
    if (a->kind == EX_CLOSURE)
        return (a->as.closure_.closure && a->as.closure_.closure->fn)
             ? a->as.closure_.closure->fn->binding : NULL;
    if (a->kind == EX_FN)
        return a->as.fn_.fn ? a->as.fn_.fn->binding : NULL;
    if (a->kind != EX_VAR || !a->as.var.binding) return NULL;
    const Binding *b = a->as.var.binding;
    if (threadable_has(b)) return b;
    if (threadable_has(b->closure_fn_binding)) return b->closure_fn_binding;
    if (threadable_has(b->hoist_closure_fn_binding)) return b->hoist_closure_fn_binding;
    return b;
}

typedef struct { const Expr *program; bool withdrew; } UnthreadedUd;

/* Does the fat closure an argument builds for a poly-fn parameter carry an
 * `fn_cps` entry?  A call through such a parameter threads ONLY through that
 * slot (cps_ir_param_call_threads), and the EX_POLY_WRAP emission
 * (emit_expr.c) fills it for two shapes: a GLOBAL fn -- named, or a lifted
 * captureless lambda -- and a capturing lambda LITERAL, each whose signature
 * fits the slot's ABI (cps_ir_fncps_sig_ok, the gate the fill itself asks).
 * Any other value (a let-bound closure, a float argument or result) is called
 * through `.fn` from a fresh root, so an effectful one must not count as
 * threaded. */
static bool arg_fat_has_fn_cps(const Expr *arg) {
    const Expr *a = arg;
    while (a && a->kind == EX_ASCRIBE) a = a->as.ascribe_.inner;
    if (!a || a->kind != EX_POLY_WRAP) return false;
    const Expr *inner = a->as.poly_wrap_.inner;
    while (inner && inner->kind == EX_ASCRIBE) inner = inner->as.ascribe_.inner;
    /* A capturing lambda: its env-box dispatcher (emit_expr.c, the closure arm
     * of EX_POLY_WRAP -- the same resolver). */
    if (a->as.poly_wrap_.is_closure)
        return emit_poly_wrap_fncps_closure(a) != NULL;
    const Binding *ib = (inner && inner->kind == EX_VAR) ? inner->as.var.binding : NULL;
    if (ib && ib->source_binding) ib = ib->source_binding;
    return ib && ib->is_global && cps_ir_fncps_sig_ok(&ib->type);
}

/* Does every call through param `p` in `fd`'s body thread the caller's
 * continuation (cps_ir_param_call_threads)?  Read after thread params are
 * registered, since registry threading depends on it. */
typedef struct { const Binding *p; bool all; } ParamCallsUd;
static bool param_calls_visit(const Expr *e, void *ud);
static void param_calls_walk(const Expr *e, ParamCallsUd *u) {
    if (!e || !u->all) return;
    if (e->kind == EX_CALL && e->as.call_.fn_binding == u->p
        && !cps_ir_param_call_threads(u->p, e))
        u->all = false;
    cps_visit_children(e, param_calls_visit, u);
}
static bool param_calls_visit(const Expr *e, void *ud) {
    param_calls_walk(e, (ParamCallsUd *)ud);
    return false;
}
static bool param_calls_all_thread(const FnDef *fd, const Binding *p) {
    if (!p || !fd || !fd->body) return false;
    ParamCallsUd u = { p, true };
    param_calls_walk(fd->body, &u);
    return u.all;
}

static bool fnval_withdraw_visit(const Expr *e, void *ud);

/* Withdraw every EFFECTFUL threadable fn-value that `e` passes to a parameter
 * some call through which does not thread (see the caller in ensure_S). */
static void fnval_withdraw_walk(const Expr *e, UnthreadedUd *u) {
    if (!e) return;
    if (e->kind == EX_CALL && e->as.call_.fn_binding) {
        const FnDef *cfd = fd_for_binding(u->program, e->as.call_.fn_binding);
        for (uint32_t k = 0; cfd && cfd->params && k < e->as.call_.n_args
                                 && k < cfd->n_params; k++) {
            const Binding *fb = arg_fnval_binding(e->as.call_.args[k]);
            if (!fb || !threadable_has(fb)) continue;
            const Binding *pk = cfd->params[k];
            if (param_calls_all_thread(cfd, pk)
                && !(pk && pk->is_poly_fn && !arg_fat_has_fn_cps(e->as.call_.args[k])))
                continue;
            /* Only an effect that ESCAPES the fn-value needs the caller's
             * handler; one it handles itself (`(fn [] (with-handler ...))`)
             * runs the same off the trampoline -- the B5 rule below. */
            const FnDef *vfd = fd_for_binding(u->program, fb);
            uint64_t lo = 0, hi = 0;
            if (vfd && vfd->body) fn_net_escaping_acc(vfd->body, &lo, &hi);
            if (!(lo || hi)) continue;
            threadable_remove(fb);
            u->withdrew = true;
        }
    }
    cps_visit_children(e, fnval_withdraw_visit, u);
}
static bool fnval_withdraw_visit(const Expr *e, void *ud) {
    fnval_withdraw_walk(e, (UnthreadedUd *)ud);
    return false;
}

/* E2c: which target fn-values are stored as a value in a `make-struct` field
 * in `e`?  An effectful fn-value stored in a struct field is called via
 * `(.field obj)` and threaded via the registry (cps_ir.c), so it must be
 * registered even though its make-struct store is not a "threadable ARG" use
 * in the param sense.  Recurses the common containers; a miss only forgoes
 * registration (conservative).  Marks each target's `stored` slot (FvMulti);
 * the walk answers false throughout, so it visits every store once. */
static bool sfs_hit(const Binding *b, const FvMulti *m) {
    int sl = fvm_slot(m, b);
    if (sl >= 0) m->stored[sl] = true;
    return false;
}

static bool expr_stores_fnval_in_struct(const Expr *e, const FvMulti *m) {
    if (!e) return false;
    switch (e->kind) {
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++) {
                const Expr *v = peel_fn_value(e->as.make_struct_.field_values[i]);
                if (v && v->kind == EX_VAR && sfs_hit(v->as.var.binding, m)) return true;
                if (expr_stores_fnval_in_struct(e->as.make_struct_.field_values[i], m)) return true;
            }
            return false;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (expr_stores_fnval_in_struct(e->as.let_.bindings[i].init, m)) return true;
            return expr_stores_fnval_in_struct(e->as.let_.body, m);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (expr_stores_fnval_in_struct(e->as.do_.items[i], m)) return true;
            return false;
        case EX_IF:
            return expr_stores_fnval_in_struct(e->as.if_.cond, m)
                || expr_stores_fnval_in_struct(e->as.if_.then_, m)
                || expr_stores_fnval_in_struct(e->as.if_.else_or_null, m);
        case EX_CALL: {
            /* A `(make-struct S ...)` lowers to a CONSTRUCTOR call (e->as.call_.ctor
             * set), so a fn-value stored in a struct field arrives here, not as
             * EX_MAKE_STRUCT.  Match a fn-value arg whose ctor field carries an
             * EFFECTFUL row -- that field is called via `(.field obj)` and threaded. */
            const CtorDef *ctor = e->as.call_.ctor;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *v = peel_fn_value(e->as.call_.args[i]);
                if (ctor && v && v->kind == EX_VAR
                    && i < ctor->n_fields
                    && !effect_row_is_empty(ctor->fields[i].effect_row)
                    && sfs_hit(v->as.var.binding, m))
                    return true;
                if (expr_stores_fnval_in_struct(e->as.call_.args[i], m)) return true;
            }
            return expr_stores_fnval_in_struct(e->as.call_.fn_expr, m);
        }
        case EX_HANDLE: {
            HandleExpr *h = e->as.handle_.handle;
            if (!h) return false;
            if (expr_stores_fnval_in_struct(h->body, m)) return true;
            for (uint8_t i = 0; i < h->n_cases; i++)
                if (expr_stores_fnval_in_struct(h->cases[i].body, m)) return true;
            return false;
        }
        /* handle-over-effectful-fn-field-in-arg-let-evicted: a builtin's
         * arguments (`println`'s), a match, a letrec.  The walker returned
         * false at these, so `(println (let [s (make-struct FE fe)] (handle
         * (.run s 3) ...)))` never registered `fe`; unregistered, an escaping
         * effectful fn-value is a permanent fiber source, and the handler and
         * performer were evicted from the CPS backend ("this effect operation
         * has no lowering here").  The same `let` outside the `println`
         * worked. */
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (expr_stores_fnval_in_struct(e->as.builtin.args[i], m)) return true;
            return false;
        /* `(.run (make-struct FE fe) 3)`: the store is the field read's
         * receiver. */
        case EX_GET_FIELD:
            return expr_stores_fnval_in_struct(e->as.get_field_.struct_expr, m);
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (expr_stores_fnval_in_struct(e->as.let_.bindings[i].init, m)) return true;
            return expr_stores_fnval_in_struct(e->as.let_.body, m);
        case EX_MATCH:
            if (expr_stores_fnval_in_struct(e->as.match_.scrutinee, m)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++)
                if (expr_stores_fnval_in_struct(e->as.match_.arms[i].body, m)) return true;
            return false;
        case EX_REINTERPRET: return expr_stores_fnval_in_struct(e->as.reinterpret_.expr, m);
        case EX_ASCRIBE: return expr_stores_fnval_in_struct(e->as.ascribe_.inner, m);
        case EX_RETURN:  return expr_stores_fnval_in_struct(e->as.return_.value, m);
        case EX_SET:     return expr_stores_fnval_in_struct(e->as.set_.value, m);
        case EX_DEF:     return expr_stores_fnval_in_struct(e->as.def_.init, m);
        case EX_WHILE:   return expr_stores_fnval_in_struct(e->as.while_.cond, m)
                             || expr_stores_fnval_in_struct(e->as.while_.body, m);
        default:         return false;
    }
}

/* r7rs-conformance-program-emits-megabytes-of-c: E2 threadability for every
 * target at once (FvMulti) -- one walk of the program, where ensure_S used to
 * walk it twice per fn-value (a per-target fn_value_threadable and
 * fnval_stored_in_struct).  For each target: `total` value-uses and `ok`
 * threadable-argument uses (threadable iff total >= 1 and total == ok), the
 * hardest tier among them, and whether it is stored in an effectful struct
 * field. */
static void fv_multi_init(FvMulti *m, const Binding *const *targets, int n) {
    memset(m, 0, sizeof *m);
    uint32_t cap = 16;
    while (cap < (uint32_t)n * 2u + 2u) cap *= 2;
    m->cap      = cap;
    m->keys     = (const Binding **)calloc(cap, sizeof *m->keys);
    m->slot     = (int *)calloc(cap, sizeof *m->slot);
    m->total    = (int *)calloc((size_t)n + 1, sizeof *m->total);
    m->ok       = (int *)calloc((size_t)n + 1, sizeof *m->ok);
    m->tier     = (int *)calloc((size_t)n + 1, sizeof *m->tier);
    m->stored   = (bool *)calloc((size_t)n + 1, sizeof *m->stored);
    m->dyn_head = (int *)malloc(((size_t)n + 1) * sizeof *m->dyn_head);
    for (int i = 0; i < n; i++) {
        m->dyn_head[i] = -1;
        const Binding *b = targets[i];
        if (!b || fvm_slot(m, b) >= 0) continue;
        uint64_t x = (uint64_t)(uintptr_t)b;
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
        uint32_t h = (uint32_t)x & (cap - 1);
        while (m->keys[h]) h = (h + 1) & (cap - 1);
        m->keys[h] = b;
        m->slot[h] = i;
    }
}

static void fv_multi_free(FvMulti *m) {
    free((void *)m->keys); free(m->slot);
    free(m->total); free(m->ok); free(m->tier); free(m->stored);
    free((void *)m->dyn_cfd); free(m->dyn_pi); free(m->dyn_next); free(m->dyn_head);
    memset(m, 0, sizeof *m);
}

/* The same item walk, and the same per-item accumulator, as the single-target
 * fn_value_threadable / fnval_stored_in_struct. */
static void fv_multi_tally(const Expr *program, FvMulti *m) {
    uint64_t dl = 0, dh = 0;
    uint32_t np = program->as.program.n;
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = program->as.program.items[i];
        if (!it) continue;
        const Expr *body = (it->kind == EX_FN_DEF && it->as.fn_def_.fn)
                         ? it->as.fn_def_.fn->body : it;
        EffAcc acc = { &dl, &dh, &dl, &dh, NULL, NULL, 0, NULL,
                       NULL, NULL, program, NULL, NULL, NULL, NULL, false, m };
        expr_collect_effects_acc(body, &acc);
        (void)expr_stores_fnval_in_struct(body, m);
    }
}

/* Target `sl`'s tier as fn_value_threadable would compute it NOW: the
 * state-independent part from the walk, raised by each capturing-lambda
 * callee's class asked against the current threadable set. */
static int fv_multi_tier(const FvMulti *m, int sl) {
    int tier = m->tier[sl];
    for (int d = m->dyn_head[sl]; d >= 0; d = m->dyn_next[d]) {
        PtClass cls = param_thread_class(m->dyn_cfd[d], m->dyn_pi[d]);
        if ((int)cls > tier) tier = (int)cls;
    }
    return tier;
}

/* E2a param->value converse: is EVERY fn-value flowing into param `pi` of `fd` a
 * REGISTERED (threadable) fn-value?  Walks all direct calls to `fd`. */
static bool param_is_thread_safe(const Expr *program, const FnDef *fd, uint32_t pi) {
    if (!program || program->kind != EX_PROGRAM || !fd || !fd->binding) return false;
    uint32_t np = program->as.program.n;
    int seen = 0;
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = program->as.program.items[i];
        if (!it) continue;
        const Expr *body = (it->kind == EX_FN_DEF && it->as.fn_def_.fn)
                         ? it->as.fn_def_.fn->body : it;
        const Expr *stack[512]; int sp = 0; stack[sp++] = body;
        while (sp > 0) {
            const Expr *e = stack[--sp];
            if (!e) continue;
            if (e->kind == EX_CALL && e->as.call_.fn_binding == fd->binding) {
                if (pi >= e->as.call_.n_args) return false;
                const Expr *a = peel_fn_value(e->as.call_.args[pi]);
                /* fn-value-fat-normalization (effect-row increment): a CAPTURING
                 * closure literal is thread-safe when its lifted lambda is
                 * threadable (the E2a fat dispatch reads its registered entry
                 * out of the box's slot 0). */
                if (a && a->kind == EX_CLOSURE) {
                    if (!(a->as.closure_.closure && a->as.closure_.closure->fn
                          && threadable_has(a->as.closure_.closure->fn->binding)))
                        return false;
                    seen++;
                } else {
                if (!a || a->kind != EX_VAR) return false;
                /* E2 (cps-tramp-resume): a SELF-recursive call passing fd's OWN
                 * param `pi` back at position `pi` threads the same row-poly
                 * fn-value -- it introduces no new fn-value, so it is trivially
                 * thread-safe (effect-poly-map).  Any OTHER arg must be a registered
                 * threadable fn-value.  A let / `^borrow`-hoist temp of a
                 * capturing closure resolves through closure_fn_binding. */
                if (!(fd->params
                      && a->as.var.binding == fd->params[pi])
                    && !threadable_has(a->as.var.binding)
                    && !threadable_has(a->as.var.binding->closure_fn_binding)
                    && !threadable_has(a->as.var.binding->hoist_closure_fn_binding))
                    return false;
                seen++;
                }
            }
            switch (e->kind) {
                case EX_CALL:
                    for (uint32_t k = 0; k < e->as.call_.n_args && sp < 510; k++)
                        stack[sp++] = e->as.call_.args[k];
                    if (sp < 511) stack[sp++] = e->as.call_.fn_expr;
                    break;
                case EX_DO:
                    for (uint32_t k = 0; k < e->as.do_.n && sp < 511; k++) stack[sp++] = e->as.do_.items[k];
                    break;
                case EX_LET:
                    for (uint32_t k = 0; k < e->as.let_.n && sp < 511; k++) stack[sp++] = e->as.let_.bindings[k].init;
                    if (sp < 511) stack[sp++] = e->as.let_.body;
                    break;
                case EX_IF:
                    if (sp < 509) { stack[sp++] = e->as.if_.cond; stack[sp++] = e->as.if_.then_; stack[sp++] = e->as.if_.else_or_null; }
                    break;
                case EX_REINTERPRET:
                    if (sp < 511) stack[sp++] = e->as.reinterpret_.expr;
                    break;
                case EX_ASCRIBE:
                    if (sp < 511) stack[sp++] = e->as.ascribe_.inner;
                    break;
                case EX_RETURN:
                    if (sp < 511) stack[sp++] = e->as.return_.value;
                    break;
                case EX_MATCH:
                    if (sp < 511) stack[sp++] = e->as.match_.scrutinee;
                    for (uint32_t k = 0; k < e->as.match_.n_arms && sp < 511; k++) stack[sp++] = e->as.match_.arms[k].body;
                    break;
                case EX_BUILTIN:
                    for (uint32_t k = 0; k < e->as.builtin.n && sp < 511; k++) stack[sp++] = e->as.builtin.args[k];
                    break;
                case EX_HANDLE:
                    if (e->as.handle_.handle) {
                        HandleExpr *h = e->as.handle_.handle;
                        if (sp < 511) stack[sp++] = h->body;
                        for (uint8_t k = 0; k < h->n_cases && sp < 511; k++) stack[sp++] = h->cases[k].body;
                    }
                    break;
                case EX_FN_DEF:
                    if (e->as.fn_def_.fn && sp < 511) stack[sp++] = e->as.fn_def_.fn->body;
                    break;
                default: break;
            }
        }
    }
    return seen >= 1;
}

/* G3b: is `fd` a colored GENERIC whose EVERY monomorph spec is CPS-admissible
 * (concrete signature ok + body core)?  Enumerated from the complete spec set
 * (ctx->abi_specializations, populated by the pre-emit scan before ensure_S).
 * `term` is the generic body's CTerm; each monomorph reuses it and resolves its
 * tyvar types through that monomorph's spec.  Returns false when the generic has
 * no monomorph specs (a dead generic taints nothing anyway). */
static bool mono_template_all_admissible(EmitCtx *ctx, const FnDef *fd, CTerm *term) {
    if (!ctx || !term) return false;
    bool any = false;
    for (uint32_t i = 0; i < ctx->n_abi_specializations; i++) {
        const EmitAbiSpecialization *spec = &ctx->abi_specializations[i];
        if (spec->fn != fd || !spec->fn_expr || !spec->clone_name) continue;
        any = true;
        const EmitAbiSpecialization *saved = ctx->current_abi_specialization;
        ctx->current_abi_specialization = (EmitAbiSpecialization *)spec;
        g_cps_mono_resolver = ctx;
        bool ok = mono_sig_ok(fd, spec) && term_core_ok(term);
        g_cps_mono_resolver = NULL;
        ctx->current_abi_specialization = saved;
        if (!ok) return false;
    }
    return any;
}

/* Bitset helpers over g_ents indices (word = index>>6, bit = index&63). */
static inline bool ent_bit(const uint64_t *bs, size_t i) {
    return (bs[i >> 6] >> (i & 63)) & 1u;
}
static inline void ent_bit_set(uint64_t *bs, size_t i) {
    bs[i >> 6] |= (uint64_t)1 << (i & 63);
}

/* Forward transitive closure over the colored call graph: grow `set` to every
 * entry reachable from a seed via `edges` (or everything, once an `edges_all`
 * conduit is reached).  Monotone; iterates to a fixed point. */
static void reach_forward(uint64_t *set, size_t n, size_t nwords) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            if (!ent_bit(set, i)) continue;
            if (g_ents[i].edges_all) {
                for (size_t j = 0; j < n; j++)
                    if (!ent_bit(set, j)) { ent_bit_set(set, j); changed = true; }
            } else if (g_ents[i].edges) {
                for (size_t w = 0; w < nwords; w++) {
                    uint64_t add = g_ents[i].edges[w] & ~set[w];
                    if (add) { set[w] |= add; changed = true; }
                }
            }
        }
    }
}

/* Backward transitive closure: grow `set` (seeded with the performers) to every
 * entry that can reach one of them -- an entry with a direct edge into `set`, or
 * one that calls indirectly (`edges_all`, so it reaches any performer). */
static void reach_backward(uint64_t *set, size_t n, size_t nwords) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t j = 0; j < n; j++) {
            if (ent_bit(set, j)) continue;
            bool reaches = g_ents[j].edges_all;
            if (!reaches && g_ents[j].edges)
                for (size_t w = 0; w < nwords && !reaches; w++)
                    if (g_ents[j].edges[w] & set[w]) reaches = true;
            if (reaches) { ent_bit_set(set, j); changed = true; }
        }
    }
}

/* A colored function whose CT-IR is a WHOLE-BODY delegation: `cps_ir_translate_fn`
 * emits it as a single `CT_LETRAW` (the entire body, direct-emitted) tailed by an
 * `appcont` to the function's own return (KK_RET).  Such a function runs its
 * effects on the FIBER runtime (the direct emitter's global_effect_handler_chain),
 * not the DK -- so any effect that ESCAPES it (its non-empty net effect) is a
 * fiber effect and must taint every DK peer sharing it, exactly like a non-in_s
 * function's effect.  Detected by shape; a control-free single-owning-op body
 * (e.g. `hamt/new`) also matches but has an empty effect set, so seeding it is a
 * no-op. */
static bool term_is_whole_body_delegation(const CTerm *t) {
    return t && t->kind == CT_LETRAW && t->as.letraw.body
        && t->as.letraw.body->kind == CT_APPCONT
        && t->as.letraw.body->as.appcont.kont.kind == KK_RET;
}

/* Flattens EX_PROGRAM items, expanding EX_DEFMODULE bodies (and top-level do)
 * into their members -- the same view the main emitter emits.  Declared here
 * (defined in emit_core.c) so the CPS classifier sees module-member fns as
 * ordinary top-level defns instead of an opaque effect-bearing module node. */
const Expr **flatten_program_items(const Expr *program, uint32_t *out_n);

static void ensure_S(const Expr *program) {
    /* Cached -- but G3b's mono-template classification needs g_emit_ctx (the
     * spec set).  If a prior run classified under a different ctx (e.g. a NULL
     * ctx from emit_cps_ir_program_has_emittable running before the first
     * try_fn), recompute so mono-templates are populated. */
    if (g_prog == program && g_ents && g_ents_ctx == g_emit_ctx) return;

    if (g_arena_live) { arena_free(&g_arena); g_arena_live = false; }
    free(g_ents); g_ents = NULL; g_ents_n = 0;
    g_prog = program;
    fdc_prog = NULL;   /* a new classification: rebuild fd_for_binding's table */
    cps_ir_callee_cache_reset();   /* ... and callee_fndef's */
    g_fwd_done = false;
    g_eff_n = 0;
    ctg_reset();
    if (!program || program->kind != EX_PROGRAM) return;

    arena_init(&g_arena, 0);
    g_arena_live = true;

    /* Coloring is idempotent (cps.c resets first); guarantee it ran. */
    cps_color_program(&g_arena, (Expr *)program);

    /* Enter EVERY colored top-level fn into the table.  Candidates (scalar sig,
     * core-only CTerm, not main/exported) start in S; non-candidates are kept
     * with in_s=false so their effect set participates in co-classification --
     * a `perform`/`handle` in a fallback function taints the effect for every
     * CPS peer, since the two effect machines (DK vs fiber) do not interoperate.
     * Uncolored functions never contain a control op, so they touch no effect
     * and are skipped entirely. */
    /* Iterate the FLATTENED program (module members hoisted) so a `(defmodule
     * ...)` member fn is classified as an ordinary top-level defn.  Without this
     * the whole EX_DEFMODULE node hit the "non-fn top-level item" catch-all
     * below, dumping EVERY module effect into base_taint and pinning every
     * module member to the fiber. */
    uint32_t np = 0;
    const Expr **items = flatten_program_items(program, &np);
    g_ents = (SEnt *)calloc(np ? np : 1, sizeof(SEnt));

    /* E2/taint-completeness pre-pass: collect every global fn whose ADDRESS is
     * taken (referenced as a fn-value anywhere in the program), so classification
     * below can force an effectful address-taken fn to the fiber.  Must run BEFORE
     * the classification loop -- a forward reference (a fn passed as a value later
     * in the file) would otherwise be missed. */
    g_addr_taken_n = 0;
    g_addr_collecting = true;
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = (Expr *)items[i];
        if (!it) continue;
        uint64_t dlo = 0, dhi = 0;   /* throwaway effect sink */
        if (it->kind == EX_FN_DEF && it->as.fn_def_.fn && it->as.fn_def_.fn->body)
            expr_collect_effects(it->as.fn_def_.fn->body, &dlo, &dhi);
        else
            expr_collect_effects(it, &dlo, &dhi);
    }
    g_addr_collecting = false;

    /* E2 threadability measurement (cps-tramp-resume): decide, for each EFFECTFUL
     * fn-value that today evicts to the fiber (an address-taken named fn or a
     * lifted lambda), whether all its value-uses are DK-threadable -- the coloring
     * that a future E2 threading channel needs.  Codegen-neutral: g_threadable_fn is
     * populated and traced only; classification below is unchanged.  Runs after the
     * address-taken pre-pass (it needs the full g_addr_taken set) and before
     * classification. */
    g_threadable_fn_n = 0;
    cps_ir_thread_param_reset();
    bool trace = getenv("TUR_TRACE_EVICT") != NULL;
    /* Every target first, then one tally walk for all of them (FvMulti), then
     * the classification in the same order as before -- a capturing-lambda
     * callee's tier is re-asked as the loop reaches each target, so a target
     * sees every threadable_add the targets before it made. */
    FnDef **tg_fd = (FnDef **)calloc(np ? np : 1, sizeof *tg_fd);
    const Binding **tg_b = (const Binding **)calloc(np ? np : 1, sizeof *tg_b);
    uint64_t *tg_eff = (uint64_t *)calloc(np ? np : 1, sizeof *tg_eff);
    int ntg = 0;
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = (Expr *)items[i];
        if (!it || it->kind != EX_FN_DEF || !it->as.fn_def_.fn) continue;
        FnDef *fd = it->as.fn_def_.fn;
        if (!fd->binding || !fd->body) continue;
        bool is_fnval = fd->binding->is_lifted_lambda || addr_taken_has(fd->binding);
        if (!is_fnval) continue;
        uint64_t lo = 0, hi = 0;
        expr_collect_effects(fd->body, &lo, &hi);
        if (!(lo || hi) && !fd->cps_colored) continue;
        tg_fd[ntg] = fd; tg_b[ntg] = fd->binding; tg_eff[ntg] = (lo || hi);
        ntg++;
    }
    FvMulti fvm;
    fv_multi_init(&fvm, tg_b, ntg);
    fv_multi_tally(program, &fvm);
    for (int ti = 0; ti < ntg; ti++) {
        FnDef *fd = tg_fd[ti];
        int sl = fvm_slot(&fvm, fd->binding);
        uint64_t lo = tg_eff[ti], hi = 0;
        /* An effectful fn-value keeps the fiber alive.  A PURE fn-value
         * normally does not -- EXCEPT one the coloring pass force-colored
         * because it flows into an EFFECTFUL fn-value param (effect-subtype
         * cluster): the HOF threads that param via the registry, which needs
         * even a pure callback to be `threadable_add`ed with a `__cps` entry,
         * else `param_is_thread_safe` fails and the HOF sig_perms "E2 pending".
         * So a colored pure lambda proceeds to the threadability check. */
        int total = fvm.total[sl], ok = fvm.ok[sl], tier = fv_multi_tier(&fvm, sl);
        bool thr = total >= 1 && total == ok;
        /* E2a: a concrete captureless fn-value is threaded onto the DK -- tier
         * `now` (tail call) OR tier `nontail` (a non-tail call, reified as a
         * heap-join frame threaded to its __cps).  Covers BOTH a lifted lambda
         * (`(fn [] (perform E))`) AND an address-taken NAMED fn (`my-eff`
         * passed by name): both are carried as a direct-entry function pointer
         * word, register (that addr -> `<name>__cps`) at startup so a threaded
         * call site (`__tur_cps_lookup((intptr_t)f)`) recovers the CPS variant
         * and the fn-value's perform reaches the caller's handler.  The named
         * case backs the handle-body threading (`run-with(my-eff)`). */
        if (thr && (tier == (int)PT_NOW || tier == (int)PT_NONTAIL)
            && (fd->binding->is_lifted_lambda || addr_taken_has(fd->binding)))
            threadable_add(fd->binding);
        /* E2c: an EFFECTFUL fn-value (lambda or named) stored in a struct
         * field is called via `(.field obj)` and threaded via the registry;
         * register it so `__tur_cps_lookup(obj.field)` resolves and its perform
         * reaches the caller's handler.  The struct-field store is not a
         * threadable-ARG use, so this is a separate admission from the E2a
         * param path above. */
        /* E2c: register a fn-value stored in an EFFECTFUL struct field
         * (fnval_stored_in_struct already checks the field row is effectful),
         * so `(.field obj)` threads to its __cps.  Includes a PURE fn stored in
         * an effectful field (effect subtyping) once the coloring pass has
         * force-colored it (so it reaches here with a __cps entry). */
        if (fvm.stored[sl] && (lo || hi || fd->cps_colored))
            threadable_add(fd->binding);
        if (trace) {
            const char *nm = fd->binding->name ? fd->binding->name->name : "?";
            const char *tiers[] = { "-", "now", "nontail", "e1" };
            fprintf(stderr, "[E2-COLOR] %-24s thr=%c tier=%-7s uses=%d ok=%d %s\n",
                    nm, thr ? 'Y' : 'N',
                    thr ? tiers[tier >= 0 && tier <= 3 ? tier : 0] : "-",
                    total, ok, fd->binding->is_lifted_lambda ? "lambda" : "named");
        }
    }
    fv_multi_free(&fvm);
    free(tg_fd); free((void *)tg_b); free(tg_eff);
    /* param->value converse: register thread-PARAMS (PT_NOW + thread-safe).
     *
     * An effectful fn-value counted threadable above is threadable because
     * every use is an argument at a threadable PARAMETER position -- a class
     * that reads only how the callee uses the parameter, not how each call
     * through it is lowered.  An empty-row call threads only through a fat
     * value's single-argument `fn_cps` slot, and an effectful-row call only via
     * the registry, which needs the parameter registered -- that is, EVERY
     * value passed to it registered (param_is_thread_safe), which a pure lambda
     * or named fn passed in another call is not.  Any other call through the
     * parameter is a plain direct call, and the effectful fn-value -- neither
     * threaded nor fiber-tainted -- performed from a fresh root: "unhandled
     * effect" at run time, from a program that compiled.  Withdraw such a
     * fn-value (cps_ir_param_call_threads says which calls thread), which
     * sends it down the E2 taint below like any other unthreadable one, and
     * re-register: a withdrawal can unthread a parameter another relied on. */
    for (int round = 0; round < 16; round++) {
        cps_ir_thread_param_reset();
        for (uint32_t i = 0; i < np; i++) {
            Expr *it = (Expr *)items[i];
            if (!it || it->kind != EX_FN_DEF || !it->as.fn_def_.fn) continue;
            FnDef *fd = it->as.fn_def_.fn;
            for (uint32_t pi = 0; pi < fd->n_params; pi++) {
                PtClass pc = param_thread_class(fd, pi);
                if ((pc == PT_NOW || pc == PT_NONTAIL)
                    && param_is_thread_safe(program, fd, pi))
                    cps_ir_thread_param_add(fd->params[pi]);
            }
        }
        UnthreadedUd wu = { program, false };
        for (uint32_t i = 0; i < np; i++) {
            const Expr *it = items[i];
            if (!it) continue;
            fnval_withdraw_walk((it->kind == EX_FN_DEF && it->as.fn_def_.fn)
                                ? it->as.fn_def_.fn->body : it, &wu);
        }
        if (!wu.withdrew) break;
    }

    /* base_taint: effects performed/handled by any top-level code that is NEVER
     * CPS-emitted -- an uncolored function (whose sole control op is hidden from
     * coloring but still emits a fiber effect op) or a top-level def initializer.
     * These permanently taint their effects. */
    uint64_t base_lo = 0, base_hi = 0;
    for (uint32_t i = 0; i < np; i++) {
        Expr *it = (Expr *)items[i];
        if (!it) continue;
        if (it->kind == EX_FN_DEF && it->as.fn_def_.fn) {
            FnDef *fd = it->as.fn_def_.fn;
            if (fd->cps_colored && fd->binding && fd->body) {
                /* `main` is the program entry point (int main(int, char**)); an
                 * exported binding names a fixed C symbol/linkage.  Neither takes
                 * the plain static int64_t entry-wrapper shape.  Exported bindings
                 * stay excluded; a delimited zero-arg `main` is admitted (D2b) with
                 * a fixed-ABI `int main` wrapper (emitted in emit_cps_ir_try_fn).
                 * Either way a `handle`/`perform` in an excluded fn still taints. */
                bool candidate = true;
                bool sig_perm = false;
                /* B6: a typeclass INSTANCE METHOD's c_export_name is an internal
                 * dict-slot name, not a user ABI export, so the CPS backend may
                 * own it (emit its exported direct entry AND a `__cps` variant).
                 * A genuine `^:export-as` export still perm-routes. */
                if (fd->binding->c_export_name
                    && !fd->binding->is_instance_method)
                    { candidate = false; sig_perm = true; }
                if (fn_is_main(fd) && !fn_is_d2b_main(fd)) { candidate = false; sig_perm = true; }
                if (candidate && !fn_sig_ok(fd)) { candidate = false; sig_perm = true; }
                /* E2/taint-completeness (cps-tramp-resume): a fn USED AS A FN-VALUE
                 * -- a lifted lambda (always a value) OR an address-taken named fn
                 * (referenced as a value in the pre-pass) -- has its DK direct-entry
                 * invoked indirectly, which installs a fresh root, so a CPS-emitted
                 * EFFECTFUL such fn's `perform` escapes (no caller handler in scope).
                 * It must run on the fiber (dynamic handler lookup); mark it a
                 * permanent fiber source so its effect taints and any DK handler-
                 * installer co-classifies to fiber.  Cleared once E2 gives fn-values
                 * a DK-threading (__fn_cps) entry. */
                if (candidate
                    && (fd->binding->is_lifted_lambda || addr_taken_has(fd->binding))
                    && !threadable_has(fd->binding)) {
                    /* B5: perm-taint only on the NET ESCAPING effect -- a perform
                     * this fn-value DISCHARGES internally (a self-handling body,
                     * e.g. the `(fn [] (handle (perform E) (E ...)))` async closure
                     * in effects-async) does not reach a fresh indirect-call root,
                     * so it is not a fiber-escape source.  Its interior handle
                     * installs its own DK prompt and CPS-lowers.  A genuinely
                     * escaping perform still taints. */
                    uint64_t lo = 0, hi = 0;
                    fn_net_escaping_acc(fd->body, &lo, &hi);
                    if (lo || hi) { candidate = false; sig_perm = true; }
                }
                CTerm *t = cps_ir_translate_fn(&g_arena, (Expr *)program, fd);
                uint32_t fresh_n = cps_ir_last_fresh_count();
                if (candidate && !term_core_ok(t)) {
                    candidate = false;
                    /* An un-lowerable inline-C form in a colored body is a
                     * PERMANENT carve-out, not a fixable BODY root: inline-C
                     * declares a fixed C signature and cannot thread a DK
                     * continuation, so no BODY-* admission can ever pull it into
                     * the CPS set.  Mark it sig_perm (like an ABI-reject
                     * signature) so it routes as a permanent SIG-* eviction and
                     * does not count against the BODY-UNSUPPORTED endgame gate;
                     * the direct emitter owns it unchanged.  Only the genuinely
                     * un-delegatable inline-C reaches here -- a whole-body-
                     * delegatable inline-C leaf (unsafe-* mains) translates to a
                     * CT_LETRAW owning-op, never a residual CT_UNSUPPORTED. */
                    const CTerm *u = first_unsupported(t);
                    if (u && u->as.unsupported.why
                        && strstr(u->as.unsupported.why, "EX_INLINE_C"))
                        sig_perm = true;
                    /* E2/taint-completeness (cps-tramp-resume): a fn evicted for an
                     * effectful fn-value call runs its effect on the fiber (the call
                     * is not DK-threadable yet).  Mark it a permanent fiber source so
                     * its effect perm-taints and any DK handler-installer of that
                     * effect co-classifies to fiber -- else the handler DK-lowers
                     * while the performer stays fiber and the effect escapes. */
                    if (u && u->as.unsupported.why
                        && strstr(u->as.unsupported.why, "effectful fn-value call (E2 pending)"))
                        sig_perm = true;
                }
                /* G3b: a colored generic sig-rejects (candidate=false) but if all
                 * its monomorphs are CPS-admissible it is a mono-template -- it
                 * stands in for its DK monomorphs and must NOT taint its effects.
                 * Only meaningful when it is not itself a direct candidate. */
                bool mono_tmpl = !candidate
                    && mono_template_all_admissible(g_emit_ctx, fd, t);
                g_ents[g_ents_n].fd = fd;
                g_ents[g_ents_n].bind = fd->binding;
                g_ents[g_ents_n].term = t;
                g_ents[g_ents_n].fresh_n = fresh_n;
                g_ents[g_ents_n].in_s = candidate;
                g_ents[g_ents_n].mono_template = mono_tmpl;
                g_ents[g_ents_n].sig_perm = sig_perm;
                SEnt *en = &g_ents[g_ents_n];
                en->perf_lo = en->perf_hi = en->hand_lo = en->hand_hi = 0;
                en->edges = NULL; en->edges_all = false;
                EffAcc acc = { &en->perf_lo, &en->perf_hi,
                               &en->hand_lo, &en->hand_hi, NULL, NULL, 0, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, false, NULL };
                expr_collect_effects_acc(fd->body, &acc);
                en->eff_lo = en->perf_lo | en->hand_lo;
                en->eff_hi = en->perf_hi | en->hand_hi;
                /* A whole-body-delegated colored fn runs its effects on the FIBER
                 * runtime (direct emitter), so its effects are fiber -- seed them
                 * into the base taint like a non-in_s fn.  This keeps an effect
                 * that a delegated HANDLER-INSTALLER discharges (e.g. `run`'s Ask)
                 * OR a delegated performer raises (e.g. `apply-cb`'s Ask via an
                 * indirect call) classified as fiber, so its evicted performers /
                 * DK peers never split across the two machines.  A control-free
                 * single-op body matches the shape but has empty effects (no-op). */
                if (candidate && term_is_whole_body_delegation(t)) {
                    base_lo |= en->eff_lo;
                    base_hi |= en->eff_hi;
                }
                g_ents_n++;
                continue;
            }
            /* uncolored (or binding-less) fn: its effects are always fiber. */
            if (fd->body) expr_collect_effects(fd->body, &base_lo, &base_hi);
            continue;
        }
        /* non-fn top-level item (e.g. a def whose init performs). */
        expr_collect_effects(it, &base_lo, &base_hi);
    }
    free((void *)items);   /* flattened view is only needed for the loops above */

    /* Build the colored call graph backing the call-path taint.  Each entry's
     * `edges` bitset names the directly-called colored peers; `edges_all` marks
     * an entry whose body makes an indirect / over-cap call (conservatively
     * reaching every colored peer). */
    size_t nwords = (g_ents_n + 63) / 64;
    if (nwords == 0) nwords = 1;
    for (size_t i = 0; i < g_ents_n; i++) {
        g_ents[i].edges = (uint64_t *)arena_alloc(&g_arena, nwords * sizeof(uint64_t));
        memset(g_ents[i].edges, 0, nwords * sizeof(uint64_t));
        g_ents[i].edges_all = false;
        if (!g_ents[i].fd || !g_ents[i].fd->body) continue;
        enum { CALLEE_CAP = 256 };
        const Binding *cbuf[CALLEE_CAP];
        int ncb = 0; bool overflow = false;
        uint64_t scratch_lo = 0, scratch_hi = 0;
        EffAcc acc = { &scratch_lo, &scratch_hi, &scratch_lo, &scratch_hi,
                       cbuf, &ncb, CALLEE_CAP, &overflow, NULL, NULL, NULL, NULL,
                       NULL, NULL, NULL, false, NULL };
        expr_collect_effects_acc(g_ents[i].fd->body, &acc);
        g_ents[i].edges_all = overflow;
        for (int k = 0; k < ncb; k++)
            for (size_t j = 0; j < g_ents_n; j++)
                if (g_ents[j].bind == cbuf[k]) { ent_bit_set(g_ents[i].edges, j); break; }
    }

    /* Precompute, per effect bit, the CALL-PATH node set: entries that lie on a
     * call path from a colored handler of the effect down to a colored performer
     * of it (forward-reachable from a handler AND backward-reachable to a
     * performer).  Fixed across the fixpoint -- only whether a node is currently a
     * fallback changes.  NULL for bits with no handler/performer pairing. */
    uint64_t *path_nodes[CPS_MAX_EFFECTS];
    for (int b = 0; b < CPS_MAX_EFFECTS; b++) {
        path_nodes[b] = NULL;
        uint64_t bl = (b < 64) ? ((uint64_t)1 << b) : 0;
        uint64_t bh = (b < 64) ? 0 : ((uint64_t)1 << (b - 64));
        bool has_h = false, has_p = false;
        for (size_t i = 0; i < g_ents_n && !(has_h && has_p); i++) {
            if ((g_ents[i].hand_lo & bl) || (g_ents[i].hand_hi & bh)) has_h = true;
            if ((g_ents[i].perf_lo & bl) || (g_ents[i].perf_hi & bh)) has_p = true;
        }
        if (!has_h || !has_p) continue;
        uint64_t *fwd = (uint64_t *)arena_alloc(&g_arena, nwords * sizeof(uint64_t));
        uint64_t *bwd = (uint64_t *)arena_alloc(&g_arena, nwords * sizeof(uint64_t));
        memset(fwd, 0, nwords * sizeof(uint64_t));
        memset(bwd, 0, nwords * sizeof(uint64_t));
        for (size_t i = 0; i < g_ents_n; i++) {
            if ((g_ents[i].hand_lo & bl) || (g_ents[i].hand_hi & bh)) ent_bit_set(fwd, i);
            if ((g_ents[i].perf_lo & bl) || (g_ents[i].perf_hi & bh)) ent_bit_set(bwd, i);
        }
        reach_forward(fwd, g_ents_n, nwords);
        reach_backward(bwd, g_ents_n, nwords);
        for (size_t w = 0; w < nwords; w++) fwd[w] &= bwd[w];  /* path = F & B */
        path_nodes[b] = fwd;
    }

    /* Fixpoint (monotone -- only removals, so it converges):
     *   Rule A: drop any candidate that needs a heap-reified join.
     *   Rule B: co-classify effects.  An effect is "tainted" if it is touched by
     *           base_taint (never-CPS code) or by any function NOT in S; evict
     *           every in-S function that touches a tainted effect, so a
     *           performer and its handler are never split across the DK and
     *           fiber machines (which do not interoperate).
     *   Rule C: call-path taint.  If any call-path node between a colored handler
     *           of E and a colored performer of E is a genuine fallback, the DK
     *           chain is severed at that cps->direct frame -- taint E so the
     *           handler + performer fall back coherently with the conduit. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < g_ents_n; i++) {
            if (g_ents[i].in_s && needs_heap_join(g_ents[i].term)) {
                g_ents[i].in_s = false;
                changed = true;
            }
            /* Rule D: an outward serial receiver must be in S itself (the
             * reset calls its `__cps` twin). */
            if (g_ents[i].in_s && !outward_receivers_in_s(g_ents[i].term)) {
                g_ents[i].in_s = false;
                changed = true;
            }
        }
        /* An effect is tainted by any entry that is NEITHER an S candidate NOR a
         * mono-template: those are the genuinely fiber functions.  A mono-template
         * stands in for its DK monomorphs, so it does not taint while it holds. */
        uint64_t taint_lo = base_lo, taint_hi = base_hi;
        for (size_t i = 0; i < g_ents_n; i++) {
            if (!g_ents[i].in_s && !g_ents[i].mono_template) {
                taint_lo |= g_ents[i].eff_lo; taint_hi |= g_ents[i].eff_hi;
            }
        }
        /* Rule C: taint an effect whose handler->performer call path routes through
         * a genuine fallback intermediary (severed DK chain). */
        for (int b = 0; b < CPS_MAX_EFFECTS; b++) {
            if (!path_nodes[b]) continue;
            bool already = (b < 64) ? ((taint_lo >> b) & 1)
                                    : ((taint_hi >> (b - 64)) & 1);
            if (already) continue;
            for (size_t i = 0; i < g_ents_n; i++) {
                if (!ent_bit(path_nodes[b], i)) continue;
                if (!g_ents[i].in_s && !g_ents[i].mono_template) {
                    if (b < 64) taint_lo |= (uint64_t)1 << b;
                    else        taint_hi |= (uint64_t)1 << (b - 64);
                    break;
                }
            }
        }
        for (size_t i = 0; i < g_ents_n; i++) {
            bool tainted = (g_ents[i].eff_lo & taint_lo) || (g_ents[i].eff_hi & taint_hi);
            if (g_ents[i].in_s && tainted) {
                g_ents[i].in_s = false;
                changed = true;
            }
            /* G3b: a mono-template whose effect is tainted by a genuine fiber peer
             * can no longer stand in for DK monomorphs -- evict it so it reverts
             * to tainting (monotone: only removals). */
            if (g_ents[i].mono_template && tainted) {
                g_ents[i].mono_template = false;
                changed = true;
            }
        }
    }

    /* PERMANENT taint (trace categorization only -- does NOT affect in_s/admission).
     * The taint that survives in a world where every BODY-fixable function is
     * admitted: only sig_perm fns (+ base code + their call-path conduits) are
     * fiber.  A fixpoint mirroring the main one, but seeded ONLY from sig_perm
     * sources: a non-sig_perm fn tainted here is FORCED fiber by a permanent
     * source and can never be admitted by BODY-* work, so it cascades into
     * perm_fiber too.  At convergence, g_perm_lo/hi holds the permanently-tainted
     * effects; the EVICT trace reports a fn evicted only via these as SIG-TAINT. */
    {
        bool *pf = (bool *)calloc(g_ents_n ? g_ents_n : 1, sizeof(bool));
        for (size_t i = 0; i < g_ents_n; i++) pf[i] = g_ents[i].sig_perm;
        uint64_t plo = base_lo, phi = base_hi;
        bool pch = true;
        while (pch) {
            pch = false;
            plo = base_lo; phi = base_hi;
            for (size_t i = 0; i < g_ents_n; i++)
                if (pf[i] && !g_ents[i].mono_template) { plo |= g_ents[i].eff_lo; phi |= g_ents[i].eff_hi; }
            for (int b = 0; b < CPS_MAX_EFFECTS; b++) {
                if (!path_nodes[b]) continue;
                bool already = (b < 64) ? ((plo >> b) & 1) : ((phi >> (b - 64)) & 1);
                if (already) continue;
                for (size_t i = 0; i < g_ents_n; i++) {
                    if (!ent_bit(path_nodes[b], i)) continue;
                    if (pf[i] && !g_ents[i].mono_template) {
                        if (b < 64) plo |= (uint64_t)1 << b; else phi |= (uint64_t)1 << (b - 64);
                        break;
                    }
                }
            }
            for (size_t i = 0; i < g_ents_n; i++) {
                bool tainted = (g_ents[i].eff_lo & plo) || (g_ents[i].eff_hi & phi);
                if (!pf[i] && !g_ents[i].mono_template && tainted) { pf[i] = true; pch = true; }
            }
        }
        g_perm_lo = plo; g_perm_hi = phi;
        free(pf);
    }

    g_ents_ctx = g_emit_ctx;   /* G3b: record the ctx this classification used */
}

static SEnt *ent_of(const FnDef *fd) {
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].fd == fd) return &g_ents[i];
    return NULL;
}

static SEnt *ent_of_binding(const Binding *b) {
    if (!b) return NULL;
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].bind == b) return &g_ents[i];
    return NULL;
}
static const FnDef *cps_fd_of_binding(const Binding *b) {
    SEnt *fe = ent_of_binding(b);
    return fe ? fe->fd : NULL;
}

/* cps-call-arm-ignores-abi-specialization: the call's OWN result type, when it
 * is concrete enough to tell two specializations apart.
 *
 * Argument types alone do not identify a specialization.  A generic whose type
 * variable reaches only the RESULT -- `bt-scope [A] [^fat body : (fn [] A)] : A`,
 * whose single parameter is the erased `int64_t` fat-thunk handle for EVERY A --
 * presents identical args at every instantiation, so the matcher below saw one
 * spec and handed it to call sites that wanted a different one (or the base).
 *
 * Returns NULL when the type decides nothing, which is the important half: a
 * call whose result is still a type variable (or an app with an abstract head)
 * c-names to the int64 carrier exactly as a genuine `int` does, and filtering on
 * that would reject the correct concrete clone for the `^Show a` wrappers RC1/RC2
 * resolve here.  Deciding nothing leaves the arg-only behaviour untouched. */
static const Type *cps_call_result_discriminator(EmitCtx *ctx,
                                                 const Expr *call_expr,
                                                 Type *store) {
    if (!ctx || !call_expr || !store) return NULL;
    Type rt = emit_resolve_type(ctx, call_expr->type);
    /* generic-call-result-in-generic-collapses-to-int: a call whose callee
     * declares a bare tyvar result is typed with the carrier `int` under its
     * tyvar wrapper, which named no spec (every by-value clone's result
     * mismatched it) and dropped `(mx-app f (mx-id x))` to the carrier base.
     * Its real result is the callee's result tyvar under the call's own
     * binding, resolved in this clone. */
    if (rt.kind == TY_INT && call_expr->kind == EX_CALL &&
        call_expr->as.call_.fn_binding &&
        call_expr->as.call_.fn_binding->type.kind == TY_FN) {
        const Type *rft = call_expr->as.call_.fn_binding->type.as.fn.result_full_type;
        if (rft && rft->kind == TY_TYVAR && rft->as.tyvar_.name) {
            for (uint8_t k = 0; k < call_expr->as.call_.n_abi_bindings; k++) {
                const AbiTypeBinding *ab = &call_expr->as.call_.abi_bindings[k];
                if (!ab->name || strcmp(ab->name, rft->as.tyvar_.name) != 0) continue;
                Type bt = emit_resolve_type(ctx, ab->type);
                if (bt.kind != TY_TYVAR && bt.kind != TY_UNKNOWN) rt = bt;
                break;
            }
        }
    }
    switch (rt.kind) {
        case TY_TYVAR:
            return NULL;
        case TY_APP:
            if (!type_app_is_concrete_adt(&rt)) return NULL;
            break;
        default:
            break;
    }
    *store = rt;
    return store;
}

/* True when `spec` produces a result the call at `want` can accept.  `want` NULL
 * (the type decided nothing) accepts every spec, so this is a pure narrowing. */
static bool spec_result_matches(EmitCtx *ctx, const EmitAbiSpecialization *spec,
                                const Type *want) {
    if (!want) return true;
    const char *wc = emit_type_c_name(ctx, *want);
    const char *sc = emit_type_c_name(ctx, spec->result_type);
    if (!wc || !sc) return true;   /* unspellable either side: do not narrow */
    return strcmp(wc, sc) == 0;
}

/* G3b: resolve a colored-generic callee at a specific CPS call site to its
 * MONOMORPH clone name, so a cps->cps tail call to a mono-template emits
 * `<clone>__cps` rather than the unresolved generic `<callee>__cps`.  Matches the
 * spec whose callee binding is `fn`, whose concrete arg types (C spelling) equal
 * the call's atom types, and whose RESULT type the call site can accept.
 * Returns NULL when 0 or >1 specs match (fall back -- never guess a wrong
 * monomorph). */
static const char *find_mono_clone_for_call(EmitCtx *ctx, const Binding *fn,
                                            const CAtom *args, uint32_t n,
                                            const Expr *call_expr) {
    if (!ctx || !fn) return NULL;
    Type want_store;
    const Type *want = cps_call_result_discriminator(ctx, call_expr, &want_store);
    const char *hit = NULL; int n_hit = 0;
    for (uint32_t i = 0; i < ctx->n_abi_specializations; i++) {
        const EmitAbiSpecialization *spec = &ctx->abi_specializations[i];
        if (spec->binding != fn || !spec->clone_name || spec->n_args != n) continue;
        if (!spec_result_matches(ctx, spec, want)) continue;
        bool ok = true;
        for (uint32_t j = 0; j < n && ok; j++) {
            if (!args[j].type) continue;   /* untyped scalar atom: not discriminating */
            if (args[j].ty != TY_APP && args[j].ty != TY_ADT && args[j].ty != TY_STRUCT) continue;
            const char *ac = emit_type_c_name(ctx, *(Type *)args[j].type);
            const char *sc = emit_type_c_name(ctx, spec->arg_types[j]);
            if (!ac || !sc || strcmp(ac, sc) != 0) ok = false;
        }
        if (ok) { hit = spec->clone_name; n_hit++; }
    }
    if (n_hit == 1) return hit;
    /* RC2/2b.3 (generic-show-wrapper-cps-monomorphization-plan): the pass above
     * SKIPS scalar args (they usually ride the int64 carrier and don't
     * discriminate).  But a `^Show a` wrapper monomorphized per SCALAR element
     * (`show_line__spec__void_bool` vs `..._const_char` vs `..._int64_t`) is
     * distinguished ONLY by its scalar arg, so when several such specs coexist the
     * skip makes them all match (n_hit > 1) and the call falls to the carrier-rep
     * base clone -- misrendering (e.g. `(show-line true)` prints `1`).  Break the
     * tie with a STRICTER pass that compares scalar args too, by concrete C type.
     * Additive: only runs when the primary pass was ambiguous, and it disqualifies
     * a spec whose arg has no concrete atom type, so a genuinely carrier-erased
     * scalar arg (atom typed as the int64 carrier) still yields no unique match
     * (returns NULL exactly as before -- no regression to non-wrapper generics). */
    if (n_hit > 1) {
        const char *hit2 = NULL; int n_hit2 = 0;
        for (uint32_t i = 0; i < ctx->n_abi_specializations; i++) {
            const EmitAbiSpecialization *spec = &ctx->abi_specializations[i];
            if (spec->binding != fn || !spec->clone_name || spec->n_args != n) continue;
            if (!spec_result_matches(ctx, spec, want)) continue;
            bool ok = true;
            for (uint32_t j = 0; j < n && ok; j++) {
                if (!args[j].type) { ok = false; break; }  /* can't discriminate */
                const char *ac = emit_type_c_name(ctx, *(Type *)args[j].type);
                const char *sc = emit_type_c_name(ctx, spec->arg_types[j]);
                if (!ac || !sc || strcmp(ac, sc) != 0) ok = false;
            }
            if (ok) { hit2 = spec->clone_name; n_hit2++; }
        }
        if (n_hit2 == 1) return hit2;
    }
    return NULL;
}

/* gcc14-int-conversion (cps->direct arg to a resolved spec-clone param): a
 * cps->direct call whose callee resolved to a monomorph/spec clone
 * (`show_line__spec__void_tur_adt_Vec__int__`) is emitted by NAME, but
 * cps_call_param_ctype reads the GENERIC binding's carrier param types (int64),
 * so a concrete-pointer clone param (`tur_adt_Vec__int *`, `const char *`) gets a
 * `(int64_t)(intptr_t)` arg cast -- a -Wint-conversion straddle on macOS clang.
 * Recover the clone's ACTUAL param C types from the ABI-spec table keyed on the
 * emitted clone name.  Returns NULL when no spec owns that name (an ordinary
 * callee -- cps_call_param_ctype's generic answer is then correct). */
static const EmitAbiSpecialization *find_spec_by_clone_name(EmitCtx *ctx,
                                                            const char *name) {
    if (!ctx || !name) return NULL;
    for (uint32_t i = 0; i < ctx->n_abi_specializations; i++) {
        const EmitAbiSpecialization *s = &ctx->abi_specializations[i];
        if (s->clone_name && strcmp(s->clone_name, name) == 0) return s;
    }
    return NULL;
}

/* True when `fn` names a COLORED top-level function (g_ents holds exactly the
 * colored fns).  Used by the island analysis: a call to a colored callee couples
 * this body to that callee's effect machine, so it disqualifies an island. */
static bool binding_is_colored(const Binding *fn) {
    if (!fn) return false;
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].bind == fn) return true;
    return false;
}

/* G3a: is the CTerm a self-contained DK ISLAND -- safe to CPS-emit as
 * `<clone>__cps` + entry wrapper with NO whole-program taint/routing change?
 * Sufficient (conservative) conditions:
 *   - every `perform` is lexically enclosed by a `handle` for its effect (no
 *     effect escapes to a caller -- an escaping dk_perform would search only the
 *     wrapper's fresh root and never reach the caller's handler);
 *   - no colored callee (CT_TAILCALL / CT_LETCALL to a colored fn -- that would
 *     couple this body to the callee's machine);
 *   - no raw reset/shift (delimited control that could escape a missing prompt).
 * `handled` carries the effect Symbols of the enclosing handlers in scope. */
static bool sym_in(const Symbol *s, const Symbol **set, int n) {
    for (int i = 0; i < n; i++) if (set[i] == s) return true;
    return false;
}
static bool is_cps_island(const CTerm *t, const Symbol **handled, int nh) {
    if (!t) return true;
    switch (t->kind) {
        case CT_APPCONT: return true;
        case CT_LETVAL:  return is_cps_island(t->as.letval.body, handled, nh);
        case CT_LETPRIM: return is_cps_island(t->as.letprim.body, handled, nh);
        case CT_LETRAW:  return is_cps_island(t->as.letraw.body, handled, nh);
        case CT_LETCALL:
            if (binding_is_colored(t->as.letcall.fn)) return false;
            return is_cps_island(t->as.letcall.body, handled, nh);
        case CT_TAILCALL:
            return !binding_is_colored(t->as.tailcall.fn);
        case CT_LETCONT:
            return is_cps_island(t->as.letcont.jbody, handled, nh)
                && is_cps_island(t->as.letcont.body, handled, nh);
        case CT_IF:
            return is_cps_island(t->as.if_.then_, handled, nh)
                && is_cps_island(t->as.if_.else_, handled, nh);
        case CT_HANDLE: {
            /* delim runs under this handle's effects (+ enclosing); continuation
             * and case bodies run outside them. */
            const Symbol *ext[32];
            int ne = nh;
            for (int i = 0; i < nh && i < 32; i++) ext[i] = handled[i];
            for (uint32_t ci = 0; ci < t->as.handle.n_cases && ne < 32; ci++)
                ext[ne++] = t->as.handle.cases[ci].effect;
            if (!is_cps_island(t->as.handle.delim, ext, ne)) return false;
            if (!is_cps_island(t->as.handle.body, handled, nh)) return false;
            for (uint32_t ci = 0; ci < t->as.handle.n_cases; ci++)
                if (!is_cps_island(t->as.handle.cases[ci].case_body, handled, nh)) return false;
            return true;
        }
        case CT_PERFORM:
            if (!sym_in(t->as.perform.effect, handled, nh)) return false;  /* escapes */
            return is_cps_island(t->as.perform.body, handled, nh);
        case CT_AWAIT:
            /* F3: await performs/handles no algebraic effect -- transparent to
             * effect co-classification; recurse the continuation. */
            return is_cps_island(t->as.await.body, handled, nh);
        case CT_RESUME:
            return is_cps_island(t->as.resume.body, handled, nh);
        case CT_CALLCC:
            /* A local setjmp escape performs/handles no effect (pure control), so
             * it is transparent to effect co-classification -- recurse the body
             * (matching the prior CT_LETRAW-delegated behavior). */
            return is_cps_island(t->as.callcc.body, handled, nh);
        case CT_LOOP:     return is_cps_island(t->as.loop.body, handled, nh);
        case CT_CONTINUE: return true;
        case CT_RESET:
        case CT_SHIFT:
        default:
            return false;   /* raw delimited control / unsupported: not an island */
    }
}

/* ---- emission -------------------------------------------------------- */

#define MAX_JOINS 64

/* ---- B7: escaping-continuation mutables as by-reference heap cells --------
 * A `^mut` that a handler case STORES its continuation into (`(set! m k)`, `k` a
 * continuation) and the program later `resume`s outlives the handler's dynamic
 * extent: `dk_perform` frees the captured chain when the case returns, so the
 * stored `k` must be cloned (copy-on-store, `dk_copy`) and the mutable must be a
 * SHARED heap cell captured BY REFERENCE into every lifted body that touches it
 * (the case that writes it, the continuation / later code that resumes it).  A
 * by-value capture would snapshot the value and lose the write.  `g_byref_muts`
 * is the set of such bindings for the function currently being emitted; the
 * mutable's C name binds the CELL POINTER (`int64_t *`), reads/writes deref it,
 * and the pointer (a scalar) rides the existing scalar-capture machinery.  Whole
 * feature that was gated on cps-tramp-resume (graduated 2026-07-19). */
static const Binding *g_byref_muts[64];
static int            g_byref_muts_n;

static bool is_byref_mut(const Binding *b) {
    if (!b) return false;
    for (int i = 0; i < g_byref_muts_n; i++) if (g_byref_muts[i] == b) return true;
    return false;
}

/* The value-position chokepoint (atom_var, emit_core.c) derefs a cell read
 * whichever emitter produces it -- a delegated `set!` VALUE expression goes
 * through the direct emitter even inside a CPS-lifted body. */
bool emit_binding_is_byref_cell(const Binding *b) { return is_byref_mut(b); }

/* The ELEMENT type of a promoted mutable's cell -- the C type the binding would
 * have as an ordinary local.  The cell must carry the mutable's real type, not a
 * uniform int64_t: a `^mut` float stored through an int64_t cell TRUNCATES at
 * every write (7.1 + 1.5 + 1.5 reads back 9, not 10.1), and a cstr would be
 * punned through an integer.  A continuation-typed mutable (B7) is int64_t here,
 * which is what its copy-on-store path already assumes. */
static const char *byref_cell_ctype(EmitCtx *ctx, const Binding *b) {
    return binder_ctype_full(ctx, b->type.kind, &b->type);
}

/* "<T> *" -- the cell POINTER spelling, arena-allocated so it can be printed. */
static const char *byref_cell_ptr_ctype(EmitCtx *ctx, const Binding *b) {
    const char *el = byref_cell_ctype(ctx, b);
    size_t n = strlen(el) + 3;
    char *s = (char *)arena_alloc(&g_arena, n);
    snprintf(s, n, "%s *", el);
    return s;
}

/* If Expr `e` (ascribe-peeled) is `(set! <mut> _)` -- a store into a mutable --
 * return the mutable target, else NULL. */
static const Binding *set_mut_target(const Expr *e) {
    while (e && e->kind == EX_ASCRIBE) e = e->as.ascribe_.inner;
    if (!e || e->kind != EX_SET || !e->as.set_.target || !e->as.set_.target->is_mut)
        return NULL;
    return e->as.set_.target;
}

/* If Expr `e` is `(set! <mut> <cont>)` -- a store of a CONTINUATION value into a
 * mutable -- return the mutable target, else NULL.  The continuation store takes
 * the copy-on-store path (dk_copy_range); a plain value store does not. */
static const Binding *byref_set_target(const Expr *e) {
    const Binding *tgt = set_mut_target(e);
    if (!tgt) return NULL;
    const Expr *se = e;
    while (se && se->kind == EX_ASCRIBE) se = se->as.ascribe_.inner;
    const Expr *v = se->as.set_.value;
    while (v && v->kind == EX_ASCRIBE) v = v->as.ascribe_.inner;
    if (v && v->kind == EX_VAR && v->as.var.binding && v->as.var.binding->is_continuation)
        return tgt;
    return NULL;
}

/* The bindings the native while-loop lowering carries as LOOP PARAMETERS: that
 * lowering owns their representation (it threads each as a parameter of the
 * emitted loop helper), so they must not also be promoted to a heap cell -- the
 * two spellings would disagree inside the helper.  Excluding them costs nothing:
 * a loop-carried mutable READ by a clause is already correct by value, because
 * the handle is re-installed each iteration, so the capture snapshot IS the
 * current iteration's value.  (A clause that WRITES a loop-carried mutable stays
 * unsupported, exactly as before -- a `cc` error, not a silent wrong answer.) */
static const Binding *g_loop_carried[64];
static int            g_loop_carried_n;

static bool is_loop_carried(const Binding *b) {
    if (!b) return false;
    for (int i = 0; i < g_loop_carried_n; i++) if (g_loop_carried[i] == b) return true;
    return false;
}

/* Collect every CT_LOOP parameter binding in the function term.  Must run over
 * the WHOLE term before any promotion decision: the loop that carries a mutable
 * can enclose the handle whose clause reads it. */
static void loop_carried_scan(const CTerm *t) {
    if (!t) return;
    switch (t->kind) {
        case CT_LOOP:
            for (uint32_t i = 0; i < t->as.loop.n_params; i++)
                if (t->as.loop.params[i].bind && g_loop_carried_n < 64
                    && !is_loop_carried(t->as.loop.params[i].bind))
                    g_loop_carried[g_loop_carried_n++] = t->as.loop.params[i].bind;
            loop_carried_scan(t->as.loop.body); return;
        case CT_LETVAL:  loop_carried_scan(t->as.letval.body);  return;
        case CT_LETPRIM: loop_carried_scan(t->as.letprim.body); return;
        case CT_LETCALL: loop_carried_scan(t->as.letcall.body); return;
        case CT_LETRAW:  loop_carried_scan(t->as.letraw.body);  return;
        case CT_LETCONT: loop_carried_scan(t->as.letcont.jbody);
                         loop_carried_scan(t->as.letcont.body); return;
        case CT_IF:      loop_carried_scan(t->as.if_.then_);
                         loop_carried_scan(t->as.if_.else_); return;
        case CT_MATCH:   for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                             loop_carried_scan(t->as.match.arms[i].body);
                         return;
        case CT_RESET:   loop_carried_scan(t->as.reset.delim);
                         loop_carried_scan(t->as.reset.body); return;
        case CT_SHIFT:   loop_carried_scan(t->as.shift.body); return;
        case CT_HANDLE:
            loop_carried_scan(t->as.handle.delim);
            loop_carried_scan(t->as.handle.body);
            for (uint32_t i = 0; i < t->as.handle.n_cases; i++)
                loop_carried_scan(t->as.handle.cases[i].case_body);
            return;
        case CT_PERFORM: loop_carried_scan(t->as.perform.body); return;
        case CT_AWAIT:   loop_carried_scan(t->as.await.body);   return;
        case CT_RESUME:  loop_carried_scan(t->as.resume.body);  return;
        case CT_CLONEABLE: loop_carried_scan(t->as.cloneable.body); return;
        case CT_CALLCC:  loop_carried_scan(t->as.callcc.body);  return;
        default: return;
    }
}

/* B7b (docs/archive/handler-clause-setbang-enclosing-mut-undeclared.md): promote every
 * ENCLOSING `^mut` a lifted handler CASE body touches -- read OR written.
 *
 * A clause is emitted as its own C function, so an enclosing mutable reaches it
 * only through the capture env, BY VALUE.  That breaks in both directions: a
 * `set!` writes a name the clause function never declared (a `cc` error naming a
 * mangled variable), and a READ silently sees the value snapshotted when the
 * handle was INSTALLED rather than the current one -- a wrong answer with no
 * diagnostic at all.  Promoting the mutable to the shared cell makes every view
 * in the function (the enclosing frame, each clause, the handle continuation)
 * read and write the same storage, which is what the source says.
 *
 * `bound` tracks bindings bound WITHIN the case body -- a mutable local to the
 * clause needs no cell (it is an ordinary local of the clause's own function). */
static void case_mut_scan(const CTerm *t, uint32_t *bound, int nb) {
    if (!t || nb >= CC_MAX_BOUND) return;
    #define MUT_B(b) do { const Binding *_b = (b); \
        if (_b && _b->is_mut && !_b->is_global && !is_byref_mut(_b) \
            && !is_loop_carried(_b)) { \
            bool _f = true; \
            for (int _i = 0; _i < nb; _i++) if (bound[_i] == _b->id) { _f = false; break; } \
            if (_f && g_byref_muts_n < 64) g_byref_muts[g_byref_muts_n++] = _b; } } while (0)
    #define MUT_A(a) do { const CAtom *_a = (a); \
        if (_a->kind == CA_VAR) MUT_B(_a->var); } while (0)
    switch (t->kind) {
        case CT_APPCONT: MUT_A(&t->as.appcont.v); return;
        case CT_LETVAL:
            MUT_A(&t->as.letval.v);
            bound[nb] = t->as.letval.x.id;
            case_mut_scan(t->as.letval.body, bound, nb + 1); return;
        case CT_LETPRIM:
            for (uint32_t i = 0; i < t->as.letprim.n; i++) MUT_A(&t->as.letprim.args[i]);
            bound[nb] = t->as.letprim.x.id;
            case_mut_scan(t->as.letprim.body, bound, nb + 1); return;
        case CT_LETCALL:
            for (uint32_t i = 0; i < t->as.letcall.n; i++) MUT_A(&t->as.letcall.args[i]);
            bound[nb] = t->as.letcall.x.id;
            case_mut_scan(t->as.letcall.body, bound, nb + 1); return;
        case CT_TAILCALL:
            for (uint32_t i = 0; i < t->as.tailcall.n; i++) MUT_A(&t->as.tailcall.args[i]);
            return;
        case CT_CONTINUE:
            for (uint32_t i = 0; i < t->as.cont_.n; i++) MUT_A(&t->as.cont_.args[i]);
            return;
        case CT_LOOP: {
            for (uint32_t i = 0; i < t->as.loop.n_params; i++) MUT_A(&t->as.loop.inits[i]);
            int nnb = nb;
            for (uint32_t i = 0; i < t->as.loop.n_params && nnb < CC_MAX_BOUND; i++)
                if (t->as.loop.params[i].bind) bound[nnb++] = t->as.loop.params[i].bind->id;
            case_mut_scan(t->as.loop.body, bound, nnb); return;
        }
        case CT_IF:
            MUT_A(&t->as.if_.cond);
            case_mut_scan(t->as.if_.then_, bound, nb);
            case_mut_scan(t->as.if_.else_, bound, nb); return;
        case CT_MATCH:
            MUT_A(&t->as.match.scrut);
            for (uint32_t ai = 0; ai < t->as.match.n_arms; ai++) {
                const CMatchArm *arm = &t->as.match.arms[ai];
                int nnb = nb;
                for (uint32_t bi = 0; bi < arm->n_fields && nnb < CC_MAX_BOUND; bi++)
                    if (arm->fields[bi]) bound[nnb++] = arm->fields[bi]->id;
                case_mut_scan(arm->body, bound, nnb);
            }
            return;
        case CT_LETCONT:
            bound[nb] = t->as.letcont.param.id;
            case_mut_scan(t->as.letcont.jbody, bound, nb + 1);
            case_mut_scan(t->as.letcont.body, bound, nb + 1); return;
        case CT_RESUME:
            MUT_A(&t->as.resume.k); MUT_A(&t->as.resume.v);
            bound[nb] = t->as.resume.x.id;
            case_mut_scan(t->as.resume.body, bound, nb + 1); return;
        case CT_PERFORM:
            for (uint32_t i = 0; i < t->as.perform.n; i++) MUT_A(&t->as.perform.args[i]);
            bound[nb] = t->as.perform.x.id;
            case_mut_scan(t->as.perform.body, bound, nb + 1); return;
        case CT_LETRAW: {
            /* A delegated Expr: its READS are its free vars; its `set!` TARGET is
             * a write, which collect_free_vars does not surface (it is not a
             * read), so take it separately -- that target is the reported bug. */
            uint32_t n_fv = 0;
            Binding **fv = collect_free_vars(t->as.letraw.e, NULL, 0, NULL, 0, &n_fv);
            for (uint32_t i = 0; i < n_fv; i++) MUT_B(fv[i]);
            free(fv);
            MUT_B(set_mut_target(t->as.letraw.e));
            bound[nb] = t->as.letraw.x.id;
            case_mut_scan(t->as.letraw.body, bound, nb + 1); return;
        }
        case CT_AWAIT:
            MUT_A(&t->as.await.fut);
            bound[nb] = t->as.await.x.id;
            case_mut_scan(t->as.await.body, bound, nb + 1); return;
        case CT_CALLCC:
            bound[nb] = t->as.callcc.x.id;
            case_mut_scan(t->as.callcc.body, bound, nb + 1); return;
        default: return;   /* nothing else appears in a handle_case_ok body */
    }
    #undef MUT_A
    #undef MUT_B
}

/* perform-inside-loop-has-no-lowering (cell-carried loop vars): inside a
 * CT_LOOP body, a delegated `(set! m v)` whose target is an enclosing `^mut`
 * that is NOT a loop parameter is a var build_loop classified as CELL-carried
 * (assigned conditionally or more than once per iteration).  Promote it to the
 * B7 shared heap cell so the helper, every lifted frame inside it, and the code
 * after the loop read and write one location.  A mutable bound INSIDE the loop
 * body (a per-iteration local) is left alone. */
static void loop_cell_scan(const CTerm *t, uint32_t *bound, int nb) {
    if (!t || nb >= CC_MAX_BOUND) return;
    #define LCS_BIND(x, body) do { bound[nb] = (x).id; loop_cell_scan((body), bound, nb + 1); } while (0)
    switch (t->kind) {
        case CT_LETRAW: {
            const Binding *tgt = set_mut_target(t->as.letraw.e);
            if (tgt && tgt->is_mut && !tgt->is_global && !is_byref_mut(tgt)
                && !is_loop_carried(tgt) && g_byref_muts_n < 64) {
                bool inner = false;
                for (int i = 0; i < nb; i++) if (bound[i] == tgt->id) { inner = true; break; }
                if (!inner) g_byref_muts[g_byref_muts_n++] = tgt;
            }
            LCS_BIND(t->as.letraw.x, t->as.letraw.body); return;
        }
        case CT_LETVAL:  LCS_BIND(t->as.letval.x, t->as.letval.body); return;
        case CT_LETPRIM: LCS_BIND(t->as.letprim.x, t->as.letprim.body); return;
        case CT_LETCALL: LCS_BIND(t->as.letcall.x, t->as.letcall.body); return;
        case CT_LETCONT:
            loop_cell_scan(t->as.letcont.body, bound, nb);
            LCS_BIND(t->as.letcont.param, t->as.letcont.jbody); return;
        case CT_IF:      loop_cell_scan(t->as.if_.then_, bound, nb);
                         loop_cell_scan(t->as.if_.else_, bound, nb); return;
        case CT_MATCH:   for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                             loop_cell_scan(t->as.match.arms[i].body, bound, nb);
                         return;
        case CT_RESET:   loop_cell_scan(t->as.reset.delim, bound, nb);
                         LCS_BIND(t->as.reset.x, t->as.reset.body); return;
        case CT_SHIFT:   loop_cell_scan(t->as.shift.body, bound, nb); return;
        case CT_HANDLE:
            loop_cell_scan(t->as.handle.delim, bound, nb);
            loop_cell_scan(t->as.handle.body, bound, nb);
            for (uint32_t i = 0; i < t->as.handle.n_cases; i++)
                loop_cell_scan(t->as.handle.cases[i].case_body, bound, nb);
            return;
        case CT_PERFORM: LCS_BIND(t->as.perform.x, t->as.perform.body); return;
        case CT_AWAIT:   LCS_BIND(t->as.await.x, t->as.await.body); return;
        case CT_RESUME:  LCS_BIND(t->as.resume.x, t->as.resume.body); return;
        case CT_CLONEABLE: loop_cell_scan(t->as.cloneable.body, bound, nb); return;
        case CT_CALLCC:  LCS_BIND(t->as.callcc.x, t->as.callcc.body); return;
        case CT_LOOP:    loop_cell_scan(t->as.loop.body, bound, nb); return;
        default: return;
    }
    #undef LCS_BIND
}

/* Populate g_byref_muts by scanning the whole function term for a delegated
 * `(set! m k)` continuation store. */
static void byref_scan(const CTerm *t) {
    if (!t) return;
    switch (t->kind) {
        case CT_LETRAW: {
            const Binding *tgt = byref_set_target(t->as.letraw.e);
            if (tgt && !is_byref_mut(tgt) && g_byref_muts_n < 64)
                g_byref_muts[g_byref_muts_n++] = tgt;
            byref_scan(t->as.letraw.body); return;
        }
        case CT_LETVAL:  byref_scan(t->as.letval.body); return;
        case CT_LETPRIM: byref_scan(t->as.letprim.body); return;
        case CT_LETCALL: byref_scan(t->as.letcall.body); return;
        case CT_LETCONT: byref_scan(t->as.letcont.jbody); byref_scan(t->as.letcont.body); return;
        case CT_IF:      byref_scan(t->as.if_.then_); byref_scan(t->as.if_.else_); return;
        case CT_MATCH:   for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                             byref_scan(t->as.match.arms[i].body);
                         return;
        case CT_RESET:   byref_scan(t->as.reset.delim); byref_scan(t->as.reset.body); return;
        case CT_SHIFT:   byref_scan(t->as.shift.body); return;
        case CT_HANDLE: {
            byref_scan(t->as.handle.delim); byref_scan(t->as.handle.body);
            for (uint32_t i = 0; i < t->as.handle.n_cases; i++) {
                /* B7b: every enclosing `^mut` this CLAUSE touches becomes a
                 * shared cell -- the clause is its own C function and cannot see
                 * the enclosing frame's locals.  The clause's own params and `k`
                 * are bound by the DKHandler signature, so seed them as bound. */
                uint32_t cb[CC_MAX_BOUND];
                int cnb = 0;
                const CHandleCase *c = &t->as.handle.cases[i];
                for (uint32_t p = 0; p < c->n_params && cnb < CC_MAX_BOUND; p++)
                    if (c->params[p]) cb[cnb++] = c->params[p]->id;
                if (c->k && cnb < CC_MAX_BOUND) cb[cnb++] = c->k->id;
                case_mut_scan(c->case_body, cb, cnb);
                byref_scan(c->case_body);
            }
            return;
        }
        case CT_PERFORM: byref_scan(t->as.perform.body); return;
        case CT_AWAIT:   byref_scan(t->as.await.body); return;
        case CT_RESUME:  byref_scan(t->as.resume.body); return;
        case CT_CLONEABLE: byref_scan(t->as.cloneable.body); return;
        case CT_CALLCC:  byref_scan(t->as.callcc.body); return;
        case CT_LOOP: {
            uint32_t lb[CC_MAX_BOUND];
            loop_cell_scan(t->as.loop.body, lb, 0);
            byref_scan(t->as.loop.body); return;
        }
        default: return;
    }
}

typedef struct {
    EmitCtx    *ctx;
    Buf        *out;         /* current function/helper body buffer */
    Buf        *helpers;     /* file-scope accumulator for lifted reset/shift helpers */
    int         indent;
    /* active inline joins: KK_VAR id -> join-parameter C name + the C type the
     * join local is DECLARED with (so a delivery can tell whether the slot is a
     * one-word carrier or the by-value aggregate itself -- see
     * deliver_slot_cty / the cps->direct aggregate bridge). */
    struct { uint32_t id; const char *param; const char *cty;
             uint32_t param_id; const Binding *param_bind; } joins[MAX_JOINS];
    int         n_joins;
    const char *cur_k;       /* C expr for the innermost prompt chain (KK_PROMPT target) */
    /* perform-inside-loop-has-no-lowering (escaping joins): joins reified as DK
     * resume-frames because a delivery to them sits inside a LIFTED region of
     * their body (a perform continuation, ...).  `frame` is the C local holding
     * the frame node in the frame that created it (`out` identifies that
     * frame); a delivery from that frame runs the node, a delivery from any
     * lifted sub-frame returns through its own downstream chain, which was
     * spliced onto the node (cur_k was the node while the body emitted). */
    struct { uint32_t id; char frame[24]; const Buf *out; } hjoins[MAX_JOINS];
    int         n_hjoins;
    const char *cur_loop_name; /* cps-while-native: enclosing CT_LOOP helper `<name>__cps`
                                * so a CT_CONTINUE back-edge (possibly inside a lifted
                                * handle continuation) re-enters it. */
    const char *cur_loop_inv;  /* cps-while-native: CSV of the enclosing loop's
                                * loop-INVARIANT extra args (free vars the loop body
                                * reads that are not carried params -- e.g. a fn param
                                * or handle result read by a loop in a handle
                                * continuation).  Appended, unchanged, after the
                                * carried args at each back-edge.  NULL if none. */
    const char *fn_cn;       /* enclosing function's mangled name (helper naming) */
    int        *helper_ctr;  /* per-function unique-helper counter */
    bool        shift_mode;  /* true inside a shift-body / handler-case helper: KK_PROMPT -> return */
    bool        ret_mode;    /* true inside a perform-continuation frame: KK_RET -> return value */
    bool        handler_case_mode;  /* E7: true directly inside a LH_HANDLER_CASE body -- a
                                     * terminal tail `resume` here emits dk_tail_resume (yield) */
    bool        case_tail_resume;   /* E7: this case was installed with dk_handler_tail (DEEP +
                                     * tail-resume); a SHALLOW case keeps the inline dk_invoke */
    /* Full Type a value has when it crosses the slot at each continuation target,
     * so Tier C by-value aggregates box/unbox with their real (monomorphized) C
     * type.  ret_ty = the function's return type (KK_RET); cur_ty = the innermost
     * prompt's result type (KK_PROMPT).  NULL => scalar (Tier A/B). */
    const Type *ret_ty;
    const Type *cur_ty;
    /* proper-tail-calls T6 (T-D6): the function is a bouncer, and `tb_out` is
     * its MAIN body buffer.  A tail dynamic call bounces only when emitted
     * there -- where `__kont` is the function's own continuation parameter --
     * never from a lifted helper, which copies this struct but writes
     * elsewhere. */
    bool        tb_bouncer;
    const Buf  *tb_out;
    /* cps-self-tail-call-relies-on-sibling-call: the function whose MAIN body
     * `self_out` is, when a self tail call there may be a backedge (NULL when
     * it may not -- a closure, a monomorph, a param held in a cell), and the
     * flag that asks for the label at the top of that body. */
    const FnDef *self_fd;
    const Buf   *self_out;
    bool        *self_backedge;
    /* cps-self-tail-call-relies-on-sibling-call (mutual recursion): the fused
     * group this function's MAIN body is rendered into (tb_out is that body),
     * its member index, and the prefix that keeps its join labels distinct
     * from the other members' inside the one C function.  NULL / "" outside a
     * group. */
    const struct CpsTcg *tcg;
    int          tcg_idx;
    const char  *lbl_pfx;
    /* closure-let-in-self-tail-loop-leaks: the boundary-reaped closure binders
     * (letraw reap_env) in scope at this point of the MAIN body (`self_out`).
     * A self tail call there is a backedge, after which none of them is read
     * again, so it frees each one then (__dk_reap_closure_now) rather than
     * leaving a loop to hold one env per turn until its outermost entry
     * returns.  Pushed and popped around the letraw's body; past the cap a
     * binder is simply left to the boundary.  A lifted helper copies this
     * struct but writes elsewhere, so it never reaches the backedge. */
    char        *loop_reaps[16];
    uint32_t     n_loop_reaps;
} CE;

/* ============================================================================
 * cps-self-tail-call-relies-on-sibling-call (mutual recursion): CPS mutual
 * tail-call groups -- proper-tail-calls T5, for `__cps` bodies.
 *
 * A self tail call in a `__cps` main body is a backedge (`__tur_cps_self`).
 * A tail call to ANOTHER CPS function was `return g__cps(args, __kont)`: a C
 * call, constant-stack only when the C compiler makes it a sibling call (gcc
 * at -O2, not -O1 or -O0).  Two procedures that call each other that way --
 * each calling a procedure variable on the way, so each is CPS -- overflowed
 * a million deep in every ASan build.
 *
 * A strongly-connected component of the cps->cps tail-call graph, two to
 * CTG_MAXMEM members, is fused into one C function,
 *   `static int64_t __cps_tcg_N(int __tcg_st, <every member's params>, DK *__kont)`,
 * that dispatches on `__tcg_st` in a `switch` at the top of a loop.  Each
 * member's rendered main body is one `case`, its parameters block locals read
 * from its slots, its join labels prefixed so they cannot collide.  A tail
 * call from a main body to any member -- itself included -- that hands on
 * `__kont` evaluates its arguments, writes the target's slots, sets
 * `__tcg_st` and jumps to the top.  `__kont` is one parameter shared by every
 * member, and a tail call does not change it.  Each member keeps
 * `<name>__cps` as a one-line wrapper into the fused function, for every call
 * from outside the group, from its own lifted helpers, and from another
 * member's non-tail position.
 *
 * The fused function is written once every member has been rendered (the
 * last one to be emitted writes it), or at the end of the translation unit if
 * one never is (emit_cps_ir_flush_groups); its members' helpers and wrappers
 * are already in the file either way.  Members are the functions the
 * self-backedge could already take -- no closure (env in param 0), no
 * monomorph, not the entry, no parameter kept in a cell or carried by a
 * loop -- and no split-library body, whose `__cps` has external linkage.
 * ========================================================================== */
#define CTG_MAXMEM   8
#define CTG_MAXSLOTS 32
typedef struct CpsTcg {
    const FnDef *mem[CTG_MAXMEM];
    int          n_mem;
    uint32_t     slot_base[CTG_MAXMEM];
    char        *slot_ctype[CTG_MAXSLOTS];
    uint32_t     n_slots;
    char        *body[CTG_MAXMEM];      /* the rendered case, NULL until rendered */
    char         name[40];
    bool         declared, defined;
} CpsTcg;

static int ctg_member_idx(const CpsTcg *g, const FnDef *fd) {
    if (!g || !fd) return -1;
    for (int i = 0; i < g->n_mem; i++) if (g->mem[i] == fd) return i;
    return -1;
}

static void ce_line(CE *ce, const char *fmt, ...) TUR_PRINTF_FMT(2, 3);
static void ce_line(CE *ce, const char *fmt, ...) {
    indent_buf(ce->out, ce->indent);
    va_list ap; va_start(ap, fmt);
    buf_vprintf(ce->out, fmt, ap);
    va_end(ap);
    buf_putc(ce->out, '\n');
}

/* cps-body-panic-not-propagated: the per-call-site panic-signal check at the
 * CPS emitter's own indent.  The rendered function returns the int64 word
 * (ctx->current_fn_ret_ctype is pinned to "int64_t" for the whole render), so
 * this is `if (tur_panicking) return ((int64_t)0);` -- the C stack unwinds to
 * the direct-entry wrapper, whose direct caller carries its own check. */
static void cps_panic_check(CE *ce) {
    int saved = ce->ctx->indent;
    ce->ctx->indent = ce->indent;
    emit_panic_signal_return(ce->ctx, ce->out);
    ce->ctx->indent = saved;
}

static const char *join_param(CE *ce, uint32_t id) {
    for (int i = ce->n_joins - 1; i >= 0; i--)
        if (ce->joins[i].id == id) return ce->joins[i].param;
    return "0";  /* unreachable when the term is well-formed */
}

/* The C type the join local for `id` was declared with, or NULL if unknown. */
static const char *join_param_cty(CE *ce, uint32_t id) {
    for (int i = ce->n_joins - 1; i >= 0; i--)
        if (ce->joins[i].id == id) return ce->joins[i].cty;
    return NULL;
}

/* Materialize an atom as a malloc'd C expression string. */
static char *atom_str(CE *ce, const CAtom *a) {
    switch (a->kind) {
        case CA_INT:  return atom_int_typed(a->i, a->ty);  /* real width -> matching *_C() suffix */
        case CA_BOOL: return atom_bool(a->b);
        case CA_UNIT: return strdup("0");
        case CA_STR:  return atom_cstr(a->str);
        case CA_FLOAT: return a->ty == TY_FLOAT32 ? atom_float32(a->f) : atom_float(a->f);
        case CA_VAR:
            /* B7: a by-reference mutable's C name binds the cell POINTER; a read
             * of its value derefs.  That happens inside atom_var -- the shared
             * value-position chokepoint -- so it applies to a delegated read in
             * the direct emitter too, not only to a CPS-IR atom. */
            return atom_var(ce->ctx, a->var);
        case CA_CVAR: return strdup(a->cvar_name ? a->cvar_name : "0");
        default:      return strdup("0");
    }
}

/* Build the RHS C expression for a supported non-println builtin. */
static char *prim_expr(const BuiltinSpec *sp, char **as, uint32_t n) {
    Buf b; buf_init(&b);
    switch (sp->shape) {
        case BS_BIN_INFIX:
            /* bit-shr is documented as a LOGICAL (unsigned) right shift;
             * see the matching comment in emit_core.c's BS_BIN_INFIX case. */
            if (strcmp(sp->c_op, ">>") == 0)
                buf_printf(&b, "(int64_t)((uint64_t)(%s) >> (uint64_t)(%s))", as[0], as[1]);
            else
                buf_printf(&b, "(%s) %s (%s)", as[0], sp->c_op, as[1]);
            break;
        case BS_VARIADIC_FOLD: {
            uint32_t opens = (n >= 2) ? n - 2 : 0;
            for (uint32_t i = 0; i < opens; i++) buf_putc(&b, '(');
            buf_printf(&b, "(%s)", as[0]);
            for (uint32_t i = 1; i < n; i++) {
                buf_printf(&b, " %s (%s)", sp->c_op, as[i]);
                if (i < n - 1) buf_putc(&b, ')');
            }
            break;
        }
        case BS_DIV_CHECK:
            /* See emit_core.c: the guard is for the integer rows only; float
             * division by zero is an IEEE 754 value, not an error. */
            if (builtin_div_is_ieee(sp)) {
                buf_printf(&b, "(%s) / (%s)", as[0], as[1]);
            } else {
                buf_printf(&b,
                    "((%s) ? ((%s) / (%s)) : "
                    "(fprintf(stderr, \"division by zero\\n\"), abort(), 0))",
                    as[1], as[0], as[1]);
            }
            break;
        case BS_PREFIX_UNARY:
            buf_printf(&b, "%s(%s)", sp->c_op, as[0]);
            break;
        case BS_AND_SC:
        case BS_OR_SC: {
            const char *op = (sp->shape == BS_AND_SC) ? "&&" : "||";
            for (uint32_t i = 0; i < n; i++) {
                if (i) buf_printf(&b, " %s ", op);
                buf_printf(&b, "(%s)", as[i]);
            }
            if (n == 0) buf_puts(&b, (sp->shape == BS_AND_SC) ? "1" : "0");
            break;
        }
        case BS_FUNC_CALL: {
            /* A plain C runtime call `c_op(args)` -- e.g. cons, the continuation
             * resume/clone/drop/serialize primitives.  Args are slot carriers. */
            /* The `cons` builtin takes any 64-bit-sized head/tail (int, cstr,
             * opaque, pointer) into its `int64_t` params -- cast through intptr_t
             * so a non-int arg (e.g. a `cstr` pattern literal in
             * `(cons "[0-9]+" ...)`) does not trip C's "incompatible pointer to
             * integer conversion" (a hard -Wint-conversion error under macOS
             * clang).  Mirrors the non-CPS emit path (emit_core.c BS_FUNC_CALL),
             * keyed on the same c_op identity. */
            bool cast_args = (sp->c_op && strcmp(sp->c_op, "cons") == 0);
            buf_printf(&b, "%s(", sp->c_op);
            for (uint32_t i = 0; i < n; i++) {
                if (i) buf_puts(&b, ", ");
                if (cast_args) {
                    buf_printf(&b, "(int64_t)(intptr_t)(%s)", as[i]);
                } else {
                    buf_printf(&b, "(%s)", as[i]);
                }
            }
            buf_putc(&b, ')');
            break;
        }
        default:
            buf_puts(&b, "0");
            break;
    }
    buf_putc(&b, '\0');
    char *s = strdup(b.data);
    buf_free(&b);
    return s;
}

static char *callee_name(const Binding *fn) {
    return raw_name_for_binding(fn);   /* malloc'd */
}

/* E2a lookup key for a `via_registry` callee.
 *
 * The registry is keyed on a function's DIRECT ENTRY address
 * (`__tur_cps_register((intptr_t)f, f__cps)`), so the key has to be that
 * address.  A plain fn-value param holds it directly.  A `^fat` param does
 * NOT: its calling convention guarantees an already-boxed
 * `{ shim, direct-entry }` fat record (the arg loop auto-shims a thin fn into
 * one), carried as the int64 pointer carrier -- so the param's own value is a
 * heap address that was never registered.  Keying on it missed every time,
 * and the miss was called unguarded: a NULL call, i.e. SIGSEGV, for every
 * `^fat` parameter carrying a non-empty effect row.  Read slot 1 instead,
 * which is exactly what `__tur_fatshim_*` dispatches through.
 *
 * Writes into `out`; returns `out` for use in a format argument. */
static const char *e2a_lookup_key(char *out, size_t cap,
                                  const Binding *fn, const char *callee) {
    if (fn && fn->is_fat)
        snprintf(out, cap, "((int64_t *)(intptr_t)(%s))[1]", callee);
    else
        snprintf(out, cap, "(intptr_t)%s", callee);
    return out;
}

/* fn-value-fat-normalization (the effect-row increment): is this via_registry
 * callee carried as a FAT handle?  Every value flowing into a normalized fn
 * param is a fat box (the elab call-site shim guarantees it), and a `^fat`
 * param always was.  A NULL binding (E2c struct-field load) keeps the raw
 * atom key -- effectful fields kept the thin store. */
static bool e2a_callee_is_fat(const Binding *fn) {
    if (!fn) return false;
    if (fn->is_fat) return true;
    return fn->is_param && fn->type.kind == TY_FN &&
           fn_param_type_is_fat_normalized(&fn->type);
}

/* The same question for either callee spelling.  An E2c struct-field LOAD
 * (fn == NULL) is fat exactly when its field is a TYPED fn field: those store
 * a fat box `{ fatshim, entry }` (the ctor's boxed-field path), and a lookup
 * keyed on the box itself found nothing -- "no CPS entry registered for
 * effectful fn-value" on every typed effectful field.  A bare `fn` field
 * keeps the thin store and the raw atom key. */
static bool e2a_call_is_fat(const Binding *fn, const CAtom *fn_atom) {
    if (fn) return e2a_callee_is_fat(fn);
    return fn_atom && fn_atom->type && fn_atom->type->kind == TY_FN &&
           fn_atom->type->as.fn.boxed;
}

/* The C type an E2a call spells for parameter `i`.  The registered `__cps`
 * twin keeps its REAL parameter types (`put__cps(double v, DK *k)`), so
 * spelling every slot `int64_t` value-converted a float argument (2.5 -> 2)
 * into the wrong register class, and the callee then read its continuation
 * from the register holding the int -- a segfault, or with a pure callee a
 * wrong answer.  When the callee has a signature, a float-class parameter
 * takes its own spelling; the word-class kinds keep the int64 carrier the
 * args are cast to (atoms_csv_call_cps), which shares their register class.
 * A signature-less callee (bare `fn` field, bare `:fn` parameter) cannot
 * carry a float at all: the elaborator rejects that at the store and the
 * call. */
static const Type *e2a_callee_sig(const Binding *fn, const CAtom *fn_atom,
                                  uint32_t n) {
    const Type *t = NULL;
    if (fn && fn->type.kind == TY_FN) t = &fn->type;
    else if (!fn && fn_atom && fn_atom->type && fn_atom->type->kind == TY_FN)
        t = fn_atom->type;
    if (!t || t->as.fn.arity != n || (n && !t->as.fn.arg_kinds)) return NULL;
    return t;
}
static const char *e2a_param_ctype(const Type *sig, uint32_t i) {
    if (!sig || i >= sig->as.fn.arity) return "int64_t";
    switch ((TypeKind)sig->as.fn.arg_kinds[i]) {
        case TY_FLOAT: case TY_FLOAT64: return "double";
        case TY_FLOAT32:                return "float";
        default:                        return "int64_t";
    }
}
/* "int64_t (*)(<lead>T0, T1, ..., DK *)" into `out`. */
static void e2a_cast(char *out, size_t cap, const char *lead, const Type *sig,
                     uint32_t n) {
    int off = snprintf(out, cap, "int64_t (*)(%s", lead);
    for (uint32_t i = 0; i < n && off < (int)cap - 64; i++)
        off += snprintf(out + off, cap - (size_t)off, "%s, ", e2a_param_ctype(sig, i));
    snprintf(out + off, cap - (size_t)off, "DK *)");
}

/* Emit the via_registry dispatch for a FAT callee.  Two box species reach an
 * effectful fn slot, distinguishable by which slot the registry knows:
 *
 *   - a capturing closure's env box `{ lifted-entry, caps... }`: slot 0 is the
 *     lifted entry, registered (when threadable) against an ENV-TAKING twin --
 *     call `twin(box, args..., kont)`;
 *   - a fatshim box `{ __tur_fatshimN, direct-entry }` (a bare fn or
 *     captureless lambda auto-shimmed by the arg loop): slot 0 is the shim
 *     (never registered), slot 1 the registered direct entry -- call
 *     `twin(args..., kont)` exactly as the thin convention did.
 *
 * Try slot 0 first, fall back to a CHECKED slot-1 lookup so a genuinely
 * unregistered value still aborts with the callee's name instead of jumping
 * into the box.  `argv` is the carrier-cast arg CSV ("" when n == 0) and
 * `thread` the continuation expression. */
static void emit_e2a_fat_dispatch(CE *ce, const char *callee, const char *who,
                                  const char *argv, uint32_t n,
                                  const char *thread, const char *tag,
                                  const Type *sig) {
    char env_cast[512];
    e2a_cast(env_cast, sizeof env_cast, "void *, ", sig, n);
    char thin_cast[512];
    e2a_cast(thin_cast, sizeof thin_cast, "", sig, n);
    ce_line(ce, "{ int64_t *__e2ab = (int64_t *)(intptr_t)(%s); /* %s (fat callee) */", callee, tag);
    ce_line(ce, "  __tur_cps_fn __e2af = __tur_cps_lookup(__e2ab[0]);");
    if (n) {
        ce_line(ce, "  if (__e2af) return ((%s)__e2af)((void *)__e2ab, %s, %s);",
                env_cast, argv, thread);
        ce_line(ce, "  return ((%s)__tur_cps_lookup_checked(__e2ab[1], \"%s\"))(%s, %s); }",
                thin_cast, who, argv, thread);
    } else {
        ce_line(ce, "  if (__e2af) return ((%s)__e2af)((void *)__e2ab, %s);",
                env_cast, thread);
        ce_line(ce, "  return ((%s)__tur_cps_lookup_checked(__e2ab[1], \"%s\"))(%s); }",
                thin_cast, who, thread);
    }
}

/* The direct `f.fn` call of a via_fncps fallback, as an int64 carrier
 * expression.  The closure's wrapper keeps the param's REAL result type, so the
 * call is spelled with it (fncps_result_kind_ok admits only the kinds below);
 * an `int64_t (*)(void*, int64_t)` cast of a `void` wrapper is a mismatched
 * call, which -fsanitize=function traps.  `args` holds the `n` argument atoms'
 * C text; a one-argument call keeps its original spelling. */
static void fncps_direct_call(Buf *out, const char *pf, char *const *args,
                              uint32_t n, const Expr *call) {
    TypeKind rk = call ? call->type.kind : TY_INT;
    Buf ps, av; buf_init(&ps); buf_init(&av);
    for (uint32_t i = 0; i < n; i++) {
        buf_puts(&ps, n == 1 ? ",int64_t" : ", int64_t");
        buf_printf(&av, ", (int64_t)(%s)", args[i]);
    }
    buf_putc(&ps, '\0'); buf_putc(&av, '\0');
    if (rk == TY_NIL)
        buf_printf(out, "(((void(*)(void*%s))%s.fn)(%s.env%s), (int64_t)0)",
                   ps.data, pf, pf, av.data);
    else if (rk == TY_BOOL)
        buf_printf(out, "((int64_t)((bool(*)(void*%s))%s.fn)(%s.env%s))",
                   ps.data, pf, pf, av.data);
    else
        buf_printf(out, "((int64_t(*)(void*%s))%s.fn)(%s.env%s)",
                   ps.data, pf, pf, av.data);
    buf_free(&ps); buf_free(&av);
}

/* The threaded call through a fat closure's `fn_cps` slot, threading `thread`.
 * The slot is declared at the one-argument ABI (`tur_poly_fn_t`, emit_module.c);
 * a twin of another arity was stored through a cast (emit_expr.c, EX_POLY_WRAP)
 * and is called back at its own type here. */
static void fncps_slot_call(Buf *out, const char *pf, char *const *args,
                            uint32_t n, const char *thread) {
    if (n == 1) {
        buf_printf(out, "%s.fn_cps(%s.env, (int64_t)(%s), %s)", pf, pf, args[0], thread);
        return;
    }
    buf_printf(out, "((int64_t (*)(void *, ");
    for (uint32_t i = 0; i < n; i++) buf_puts(out, "int64_t, ");
    buf_printf(out, "DK *))%s.fn_cps)(%s.env, ", pf, pf);
    for (uint32_t i = 0; i < n; i++) buf_printf(out, "(int64_t)(%s), ", args[i]);
    buf_printf(out, "%s)", thread);
}

/* The atom text of a via_fncps call's arguments (malloc'd array of malloc'd
 * strings; free with fncps_args_free). */
static char **fncps_args_str(CE *ce, const CAtom *args, uint32_t n) {
    char **v = (char **)calloc(n ? n : 1, sizeof(char *));
    if (!v) { fprintf(stderr, "tur: oom\n"); abort(); }
    for (uint32_t i = 0; i < n; i++) v[i] = atom_str(ce, &args[i]);
    return v;
}
static void fncps_args_free(char **v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* Join a term's atom arguments into a malloc'd "a0, a1, ..." string. */
static char *atoms_csv(CE *ce, const CAtom *args, uint32_t n) {
    Buf b; buf_init(&b);
    for (uint32_t i = 0; i < n; i++) {
        if (i) buf_puts(&b, ", ");
        char *a = atom_str(ce, &args[i]);
        buf_puts(&b, a);
        free(a);
    }
    buf_putc(&b, '\0');
    char *s = strdup(b.data);
    buf_free(&b);
    return s;
}

/* Like atoms_csv, but for CALL-argument positions where a bare `TY_FN`
 * fn-value flows into an HOF whose fn-value param is declared `int64_t` (the
 * threading ABI carries fn-values as int64).  The lifted lambda is emitted as a
 * plain C function pointer (`void (*)(...)`), so passing it raw is an
 * int-from-pointer warning; cast it through intptr_t.  A FAT closure
 * (`tur_poly_fn_t`) is a struct value, not a bare pointer, so it is left
 * untouched.  Non-fn atoms are emitted exactly as atoms_csv would, so any call
 * with no fn-value arg is byte-identical. */
static char *atoms_csv_call(CE *ce, const CAtom *args, uint32_t n) {
    Buf b; buf_init(&b);
    for (uint32_t i = 0; i < n; i++) {
        if (i) buf_puts(&b, ", ");
        char *a = atom_str(ce, &args[i]);
        if (!atom_is_fat_fn(&args[i]) &&
            (args[i].kind == CA_VAR || args[i].kind == CA_CVAR) &&
            args[i].ty == TY_FN)
            buf_printf(&b, "(int64_t)(intptr_t)%s", a);
        else
            buf_puts(&b, a);
        free(a);
    }
    buf_putc(&b, '\0');
    char *s = strdup(b.data);
    buf_free(&b);
    return s;
}

/* atoms_csv_call for a RESOLVED clone (a mono-clone / re-resolved method):
 * its parameters are the concrete types, which an atom usually already has --
 * but not one the translator atomized out of a carrier-typed operand (a
 * generic call's argument reinterpreted `cstr -> int` for the base ABI,
 * passed on to the `g__spec__const_char__` clone).  Bridge a variable whose
 * RECORDED C type differs from the clone's recorded parameter type across the
 * word boundary; everything else passes as atoms_csv_call passes it. */
static char *atoms_csv_call_clone(CE *ce, const CAtom *args, uint32_t n,
                                  const char *clone) {
    Buf b; buf_init(&b);
    for (uint32_t i = 0; i < n; i++) {
        if (i) buf_puts(&b, ", ");
        char *a = atom_str(ce, &args[i]);
        const char *pc = clone ? emit_sig_lookup_param_ctype(clone, i) : NULL;
        const char *ac = (args[i].kind == CA_CVAR || args[i].kind == CA_VAR) &&
                         emit_str_is_bare_ident(a)
            ? emit_localvar_lookup_ctype(a) : NULL;
        bool bridge = pc && ac && strcmp(pc, ac) != 0 &&
                      (strcmp(ac, "int64_t") == 0 || strcmp(pc, "int64_t") == 0) &&
                      !atom_is_fat_fn(&args[i]);
        if (bridge) {
            size_t PL = strlen(pc), AL = strlen(ac);
            bool pp = PL && pc[PL - 1] == '*', ap = AL && ac[AL - 1] == '*';
            if (pp || ap) emit_scalar_word_conv(&b, ac, pc, a);
            else buf_puts(&b, a);
        } else if (!atom_is_fat_fn(&args[i]) &&
                   (args[i].kind == CA_VAR || args[i].kind == CA_CVAR) &&
                   args[i].ty == TY_FN) {
            buf_printf(&b, "(int64_t)(intptr_t)%s", a);
        } else {
            buf_puts(&b, a);
        }
        free(a);
    }
    buf_putc(&b, '\0');
    char *s = strdup(b.data);
    buf_free(&b);
    return s;
}

/* gcc14-int-conversion: the E2a threaded-fn-value dispatch calls through a
 * synthesized `int64_t (*)(int64_t..., DK *)` pointer, so EVERY argument slot is
 * the int64 carrier.  Unlike the ordinary call CSV (which passes each arg in its
 * natural C type to a real callee), here a pointer-like arg -- a cstr literal, a
 * `ptr<void>`, an rc/weak/ref handle, a fn value, a continuation -- must be cast
 * to `(int64_t)(intptr_t)`, else it "makes integer from pointer" at the call, a
 * hard error under GCC >= 14.  int/bool pass through as int64 already.  A float
 * arg passes bare, and the E2a cast spells its slot `double` (e2a_param_ctype);
 * "float args do not occur on this carrier path" was never true. */
static bool atom_ty_is_ptr_carrier(TypeKind k) {
    return k == TY_FN || k == TY_PTR_VOID || k == TY_CSTR || k == TY_RC ||
           k == TY_WEAK || k == TY_REF || k == TY_REF_IMMUT || k == TY_REF_MUT ||
           k == TY_CONT || k == TY_CLONEABLE_CONT || k == TY_FORALL;
}
static char *atoms_csv_call_cps(CE *ce, const CAtom *args, uint32_t n) {
    Buf b; buf_init(&b);
    for (uint32_t i = 0; i < n; i++) {
        if (i) buf_puts(&b, ", ");
        char *a = atom_str(ce, &args[i]);
        if (!atom_is_fat_fn(&args[i]) && atom_ty_is_ptr_carrier(args[i].ty))
            buf_printf(&b, "(int64_t)(intptr_t)%s", a);
        else
            buf_puts(&b, a);
        free(a);
    }
    buf_putc(&b, '\0');
    char *s = strdup(b.data);
    buf_free(&b);
    return s;
}

/* gcc14-int-conversion (carrier-to-typed-param, CPS path): the C type the callee
 * declares for parameter `i`, matching emit_params (the `__cps`/direct signature
 * emitter) EXACTLY so the argument cast agrees with the declared param.  A `__cps`
 * callee's params are NOT uniformly int64 -- some are `void *`, some concrete
 * `tur_adt_X *` -- so a blanket int64 cast is wrong (it makes int64->void* at a
 * void* param).  Returns NULL for a param whose C type is an aggregate/poly-fn
 * struct (not intptr_t-castable) or when the callee's param types are unknown. */
/* effectful-fnvalue-param-miscompile (E2): true when the callee declares
 * parameter `i` as a rank-2 poly fn -- its C type is the multi-word
 * `tur_poly_fn_t` aggregate, taken BY VALUE.  A `tur_poly_fn_t` argument must
 * pass bare (not `(int64_t)(intptr_t)arg`, which is "aggregate used where an
 * integer was expected"). */
static bool cps_call_param_is_poly_fn(CE *ce, const Binding *fn, uint32_t i) {
    (void)ce;
    if (!fn || fn->type.kind != TY_FN || i >= fn->type.as.fn.arity) return false;
    SEnt *fe = ent_of_binding(fn);
    const FnDef *fd = fe ? fe->fd : NULL;
    return fd && i < fd->n_params && fd->params[i]->is_poly_fn;
}

static const char *cps_call_param_ctype(CE *ce, const Binding *fn, uint32_t i) {
    if (!fn || fn->type.kind != TY_FN || i >= fn->type.as.fn.arity) return NULL;
    SEnt *fe = ent_of_binding(fn);
    const FnDef *fd = fe ? fe->fd : NULL;
    if (fd && i < fd->n_params) {
        if (fd->params[i]->is_poly_fn) return NULL;           /* tur_poly_fn_t */
        if (fd->params[i]->type.kind == TY_FN) return "int64_t";
        return binder_ctype_full(ce->ctx, fd->params[i]->type.kind,
                                 &fd->params[i]->type);
    }
    if (fn->type.as.fn.arg_full_types && fn->type.as.fn.arg_full_types[i]) {
        const Type *pt = fn->type.as.fn.arg_full_types[i];
        if (pt->kind == TY_FN) return "int64_t";
        return binder_ctype_full(ce->ctx, pt->kind, pt);
    }
    return NULL;
}

/* CSV of call args, each cast to its DECLARED param C type (via intptr_t) for a
 * cps->cps call whose `__cps` callee has known param types.  Only casts a param
 * whose C type is the int64 carrier or a pointer (`... *`); an aggregate/poly-fn
 * param is left to the plain-atom emission (never intptr_t-cast).  Falls back to
 * the plain atom for a param we cannot type. */
/* `offs`, when non-NULL, receives each argument's start offset in the
 * returned string (n entries; ", " separates them), so a caller can take the
 * arguments one at a time -- the self tail call's backedge does. */
/* Is `a` a bare C identifier whose recorded C type is a pointer? */
static bool cps_atom_recorded_ptr(const char *a) {
    if (!a || !emit_str_is_bare_ident(a)) return false;
    const char *c = emit_localvar_lookup_ctype(a);
    size_t L = c ? strlen(c) : 0;
    return L >= 1 && c[L - 1] == '*';
}
static char *atoms_csv_call_typed_offs(CE *ce, const CAtom *args, uint32_t n,
                                       const Binding *fn,
                                       const EmitAbiSpecialization *spec,
                                       size_t *offs) {
    Buf b; buf_init(&b);
    for (uint32_t i = 0; i < n; i++) {
        if (i) buf_puts(&b, ", ");
        if (offs) offs[i] = b.len;
        char *a = atom_str(ce, &args[i]);
        const char *pty = cps_call_param_ctype(ce, fn, i);
        /* When the callee resolved to a monomorph/spec clone, cps_call_param_ctype
         * still reports the GENERIC binding's carrier param (int64/void*); prefer
         * the clone's CONCRETE param type from the spec when it is a pointer, so a
         * concrete-pointer param (`tur_adt_Vec__int *`, `const char *`) gets the
         * matching `(T *)(intptr_t)` bridge instead of an int64 straddle. */
        if (spec && i < spec->n_args) {
            const char *sty = emit_type_c_name(ce->ctx, spec->arg_types[i]);
            size_t sL = sty ? strlen(sty) : 0;
            if (sty && sL >= 1 && sty[sL - 1] == '*')
                pty = sty;
            /* The clone's recorded signature, when it has one: a by-value
             * aggregate or float parameter of the clone is exactly that, not
             * the generic binding's carrier word (the boxing and bit rules
             * below key on an int64 parameter). */
            const char *sp = spec->clone_name
                ? emit_sig_lookup_param_ctype(spec->clone_name, i) : NULL;
            if (sp) pty = sp;
        } else if (fn) {
            /* cps-direct-arg-follows-emitted-signature: an unspecialized callee
             * (a carrier base, an inline-C primitive) is emitted with its
             * carrier parameters, while cps_call_param_ctype resolves the
             * GENERIC annotation through the ACTIVE spec -- `vec-push-ex`'s
             * `(Vec A)` came back `tur_adt_Vec__Result__int__int *` inside a
             * `(Result int int)` clone, and the receiver was cast to it for an
             * `int64_t` parameter (an int-conversion error under clang and gcc
             * 14).  The recorded emitted signature is the callee's truth. */
            char *fcn = raw_name_for_binding(fn);
            const char *sp = fcn ? emit_sig_lookup_param_ctype(fcn, i) : NULL;
            if (sp) pty = sp;
            free(fcn);
        }
        size_t L = pty ? strlen(pty) : 0;
        bool param_is_ptr = pty && L >= 1 && pty[L - 1] == '*';
        bool param_is_i64 = pty && strcmp(pty, "int64_t") == 0;
        bool param_is_voidp = pty && strcmp(pty, "void *") == 0;
        bool arg_is_ptr = atom_ty_is_ptr_carrier(args[i].ty);
        /* An arg whose C representation is provably a plain integer (not a pointer
         * carried on the int64 slot): a plain int/bool literal or scalar.  Such an
         * arg into an int64 param needs no cast; a heap/pointer arg (TY_APP/ADT,
         * cstr, ...) into an int64 param does. */
        bool arg_is_plain_int = (args[i].ty == TY_INT || args[i].ty == TY_BOOL ||
            args[i].ty == TY_INT8 || args[i].ty == TY_INT16 ||
            args[i].ty == TY_INT32 || args[i].ty == TY_INT64 ||
            args[i].ty == TY_UINT8 || args[i].ty == TY_UINT16 ||
            args[i].ty == TY_UINT32 || args[i].ty == TY_UINT64 ||
            args[i].ty == TY_NIL);
        /* Cast each arg to the callee's declared param C type -- EXCEPT the two
         * provably-valid no-cast cases, so matching args stay bare (minimal
         * snapshot churn): (1) a pointer-like arg into a `void *` param (any object
         * pointer implicitly converts to void*), and (2) an int64-ish arg into an
         * int64 param handled by falling through.  Everything else that is a
         * mismatch -- a heap/pointer arg into an int64 slot, an int64-carried arg
         * into a `void *` / concrete pointer param -- is bridged through intptr_t
         * (value-preserving). */
        /* A by-value aggregate arg (a `tur_adt_Option__int` struct value) is NOT a
         * scalar -- `(int64_t)(intptr_t)(agg)` is "aggregate used where integer
         * expected".  Its concrete->carrier crossing is a spill+address bridge, not
         * this cast, so leave it to the existing atom emission.  An arg is such an
         * aggregate iff its own C type is neither `int64_t` nor a pointer; a TY_FN
         * value is a scalar (int64/fn-ptr carrier) and stays castable. */
        bool arg_is_byval_agg = false;
        if (args[i].ty != TY_FN && args[i].type) {
            const char *acty = binder_ctype_full(ce->ctx, args[i].ty, args[i].type);
            size_t aL = acty ? strlen(acty) : 0;
            arg_is_byval_agg = acty && strcmp(acty, "int64_t") != 0 &&
                               !(aL >= 1 && acty[aL - 1] == '*');
        }
        /* Whether the arg's ACTUAL C expression type is a pointer.  arg_is_ptr
         * above is a SEMANTIC "this Turmeric value is heap/pointer-like" check;
         * it stays true for a value carried on the int64 slot (a fn carrier, a
         * boxed handle spilled to `int64_t __t4`).  For a `void *` param the bare
         * "object ptr -> void*" pass is only valid when the C expression is really
         * a pointer -- passing an `int64_t`-typed carrier bare into a `void *`
         * param is a -Wint-conversion straddle (macOS clang hard error).  When the
         * arg's C type is knowable and is NOT a pointer, force the bridge below. */
        const char *arg_cty = args[i].type
            ? binder_ctype_full(ce->ctx, args[i].ty, args[i].type) : NULL;
        size_t arg_cty_L = arg_cty ? strlen(arg_cty) : 0;
        bool arg_c_ptr_known = arg_cty != NULL;
        bool arg_is_c_ptr = arg_cty && arg_cty_L >= 1 &&
                            arg_cty[arg_cty_L - 1] == '*';
        /* gcc14-int-conversion (carrier-to-typed-param): a fat-fn arg (a `void *`
         * fat-closure carrier) passed into a callee slot the signature declares
         * as the int64 fn-carrier (`int64_t cmp_fn`, e.g. `option-eq?`'s
         * comparator) is a void*->int64 -Wint-conversion -- a hard error under
         * GCC >= 14.  Bridge it to the carrier; the fat carrier's bits are
         * preserved.  A fat-fn into a genuine fn-value slot (`void *` /
         * tur_poly_fn_t param) is left bare below. */
        if (atom_is_fat_fn(&args[i]) && param_is_i64)
            buf_printf(&b, "(int64_t)(intptr_t)%s", a);
        else if (cps_call_param_is_poly_fn(ce, fn, i))
            buf_puts(&b, a);                 /* E2: tur_poly_fn_t param -- pass the fat struct by value */
        else if (arg_is_byval_agg && param_is_i64 && arg_cty &&
                 strcmp(arg_cty, "tur_tagged_t") == 0) {
            /* saffron-cps-vec-element-carrier-mismatch, the ARGUMENT direction.
             *
             * The branch below is right that a by-value aggregate's
             * concrete->carrier crossing is "a spill+address bridge, not this
             * cast" -- but for a `tur_tagged_t` into a carrier-shaped slot
             * (`vec-push!`'s `int64_t val`) nothing downstream performed that
             * bridge, so the 16-byte struct was passed bare where an `int64_t`
             * was declared: a hard error, and the second half of why a Saffron
             * function that both performed and touched a vector did not build.
             *
             * So do the bridge here, spelled exactly as the DIRECT emitter
             * spells it at the same site -- heap-box the value and pass the
             * address -- which is what makes the element readable back as an
             * `any` (and what the return direction, in the CT_LETCALL arm,
             * dereferences).  The box is the element's, owned by the container
             * for as long as it holds it, so it is deliberately not reaped:
             * freeing it here would be a use-after-free on the next read. */
            int abx = ce->ctx->tmp_n++;
            ce_line(ce, "tur_tagged_t *__anybox_%d = (tur_tagged_t *)malloc(sizeof(tur_tagged_t));", abx);
            ce_line(ce, "*__anybox_%d = %s;", abx, a);
            buf_printf(&b, "(int64_t)(intptr_t)__anybox_%d", abx);
        }
        else if (arg_is_byval_agg && param_is_i64 && arg_cty &&
                 strncmp(arg_cty, "tur_adt_", 8) == 0) {
            /* cps-direct-arg-follows-emitted-signature: the same bridge for any
             * by-value aggregate into an int64 carrier slot -- the direct
             * emitter's argument chain heap-boxes it for a carrier parameter
             * (`(vec-push! w (app f x))` at `A := (Result int int)` passed the
             * struct bare: invalid C).  Not reaped, for the reason above: a
             * container that keeps the word owns the box. */
            int abx = ce->ctx->tmp_n++;
            ce_line(ce, "%s *__aggbox_%d = (%s *)malloc(sizeof(%s));",
                    arg_cty, abx, arg_cty, arg_cty);
            ce_line(ce, "*__aggbox_%d = %s;", abx, a);
            ce_line(ce, "TUR_REGION_NOTE_WORDS(__aggbox_%d, sizeof *__aggbox_%d);", abx, abx);
            buf_printf(&b, "(int64_t)(intptr_t)__aggbox_%d", abx);
        }
        else if (param_is_i64 && arg_cty &&
                 (strcmp(arg_cty, "double") == 0 || strcmp(arg_cty, "float") == 0)) {
            /* A float into an int64 carrier slot crosses as its BITS; the
             * intptr_t cast below would value-convert it. */
            Type ft = emit_type_from_kind(strcmp(arg_cty, "float") == 0 ? TY_FLOAT32
                                                                        : TY_FLOAT);
            char *w = emit_word_slot_bits(&ft, a);
            buf_puts(&b, w);
            free(w);
        }
        else if (param_is_i64 && !arg_is_byval_agg && !atom_is_fat_fn(&args[i]) &&
                 (arg_is_c_ptr || cps_atom_recorded_ptr(a))) {
            /* cps-typed-pointer-into-carrier-slot: the atom's elaborated kind
             * can be the int carrier while its C binder is a typed pointer --
             * `(let [v (vec-new)] ...)` at A := float binds
             * `tur_adt_Vec__float * v`, then `vec-push-ex`'s `int64_t v` got
             * it bare (arg_is_plain_int said "no cast"): a hard
             * -Wint-conversion error under clang and gcc 14.  The C type
             * decides, as in the direct emitter's argument chain. */
            buf_printf(&b, "(int64_t)(intptr_t)%s", a);
        }
        else if (atom_is_fat_fn(&args[i]) || arg_is_byval_agg)
            buf_puts(&b, a);
        else if (param_is_voidp && arg_is_ptr && (!arg_c_ptr_known || arg_is_c_ptr))
            buf_puts(&b, a);                 /* real object ptr -> void*: already valid */
        else if (param_is_voidp)
            buf_printf(&b, "(void *)(intptr_t)%s", a);  /* int64 carrier -> void*: bridge */
        else if (param_is_ptr)
            buf_printf(&b, "(%s)(intptr_t)%s", pty, a);
        else if ((param_is_i64 || !pty) && !arg_is_plain_int)
            buf_printf(&b, "(int64_t)(intptr_t)%s", a);
        else
            buf_puts(&b, a);
        free(a);
    }
    buf_putc(&b, '\0');
    char *s = strdup(b.data);
    buf_free(&b);
    return s;
}
static char *atoms_csv_call_typed(CE *ce, const CAtom *args, uint32_t n,
                                  const Binding *fn,
                                  const EmitAbiSpecialization *spec) {
    return atoms_csv_call_typed_offs(ce, args, n, fn, spec, NULL);
}

static void emit_term(CE *ce, const CTerm *t);
static void emit_binder_decls(CE *ce, const CTerm *t);
static const char *emit_param_ctype(EmitCtx *ctx, const FnDef *fd, uint32_t i);
static void emit_reset(CE *ce, const CTerm *t);
static void emit_shift(CE *ce, const CTerm *t);
static void emit_cloneable(CE *ce, const CTerm *t);
static void emit_handle(CE *ce, const CTerm *t);
static void emit_perform(CE *ce, const CTerm *t);
static void emit_await(CE *ce, const CTerm *t);   /* F3 (cps-async) */
static void emit_resume(CE *ce, const CTerm *t);
static void emit_letraw(CE *ce, const CTerm *t);
static void emit_letraw_fnfield_reap(CE *ce, const CTerm *t, const char *bn);
static void emit_callcc(CE *ce, const CTerm *t);
static void emit_heap_join(CE *ce, const CTerm *t);
static void emit_escaping_join(CE *ce, const CTerm *t);  /* escaping joins */
static void emit_loop(CE *ce, const CTerm *t);       /* cps-while-native */
static void emit_continue(CE *ce, const CTerm *t);   /* cps-while-native */
static void emit_match(CE *ce, const CTerm *t);      /* B4 */
static char *emit_cont_env(CE *ce, const char *hname, const CapSet *caps);
static char *cvar_cname(CE *ce, CVar x);

/* The full Type a value has when delivered to continuation `kont` (the target's
 * result type), or NULL for a scalar / inline join.  Used as a fallback when the
 * delivered value's own Type is not in hand (a cps->direct tail result). */
static const Type *deliver_ty(CE *ce, const CKont *kont) {
    if (kont->kind == KK_RET)    return ce->ret_ty;
    if (kont->kind == KK_PROMPT) return ce->cur_ty;
    return NULL;   /* KK_VAR: inline join, a plain C-local assignment */
}

/* True when C type spelling `cty` names a BY-VALUE AGGREGATE -- an ADT/struct
 * record passed and returned as a C struct (`tur_adt_Result__int__int`), as
 * opposed to a scalar, a pointer, or the uniform int64 carrier.  Keyed on the
 * spelling rather than a Type because the sig side table (the source of truth
 * for what a monomorph/spec clone actually returns) carries only the string.
 * Positive-list on purpose: everything not recognizably an aggregate is treated
 * as a word, which is the historical behavior. */
static bool cty_is_byval_agg(const char *cty) {
    if (!cty || !*cty) return false;
    size_t L = strlen(cty);
    if (cty[L - 1] == '*') return false;              /* a handle, not a value */
    return strncmp(cty, "tur_adt_", 8) == 0;
}

/* True when one of two C type spellings is a pointer and the other the int64
 * carrier word -- an assignment between them needs an intptr_t bridge. */
static bool cty_is_ptr(const char *cty) {
    return cty && *cty && cty[strlen(cty) - 1] == '*';
}
static bool cty_is_int_word(const char *cty) {
    return cty && (strcmp(cty, "int64_t") == 0 || strcmp(cty, "intptr_t") == 0);
}
static bool cty_word_straddle(const char *a, const char *b) {
    return (cty_is_ptr(a) && cty_is_int_word(b)) || (cty_is_int_word(a) && cty_is_ptr(b));
}

/* The C type of the SLOT that a delivery to `kont` lands in: the join local's
 * declared type (KK_VAR), or -- for the one-word DK slot -- the type the
 * crossing value is expected to have there, which is what slot_store_reap keys
 * its Tier-C boxing on.  NULL when not knowable (treated as a word). */
static const char *deliver_slot_cty(CE *ce, const CKont *kont) {
    if (kont->kind == KK_VAR) return join_param_cty(ce, kont->id);
    const Type *vty = deliver_ty(ce, kont);
    return vty ? binder_ctype_full(ce->ctx, vty->kind, vty) : NULL;
}

/* Deliver value-string `v` to continuation `kont`.  A Tier C by-value aggregate
 * is boxed into the slot on the way out.  `explicit_vty` is the delivered value's
 * own Type when the caller has it (a CT_APPCONT atom) -- it is the most precise
 * source (e.g. a cross-function shift delivers to a prompt whose result type the
 * enclosing function's cur_ty does not carry); NULL falls back to deliver_ty. */
static void emit_deliver_ty(CE *ce, const CKont *kont, const char *v, const Type *explicit_vty) {
    const Type *vty = explicit_vty ? explicit_vty : deliver_ty(ce, kont);
    if (kont->kind == KK_RET) {
        /* In a perform-continuation frame the delivered value IS the result --
         * return it (dk_perform routes it to the handler's outer continuation);
         * otherwise run the function's return continuation. */
        char *sv = slot_store_reap(ce->ctx, kont->ty, vty, v);
        if (ce->ret_mode)
            ce_line(ce, "return %s;", sv);
        else
            ce_line(ce, "return dk_run(__kont, %s);", sv);
        free(sv);
    } else if (kont->kind == KK_PROMPT) {
        /* Deliver to the innermost prompt.  In a shift-body helper the delivered
         * value IS the shift result -- return it; otherwise run the prompt chain. */
        char *sv = slot_store_reap(ce->ctx, kont->ty, vty, v);
        if (ce->shift_mode)
            ce_line(ce, "return %s;", sv);
        else
            ce_line(ce, "return dk_run(%s, %s);", ce->cur_k, sv);
        free(sv);
    } else {
        /* perform-inside-loop-has-no-lowering: a join reified as a resume-frame
         * (emit_escaping_join).  From the frame that created it, run the node;
         * from a lifted sub-frame the node IS this frame's downstream chain
         * (it was cur_k when the sub-frame was spliced), so deliver exactly as
         * a KK_RET would. */
        for (int i = ce->n_hjoins - 1; i >= 0; i--) {
            if (ce->hjoins[i].id != kont->id) continue;
            char *sv = slot_store_reap(ce->ctx, kont->ty, vty, v);
            if (ce->hjoins[i].out == ce->out)
                ce_line(ce, "return dk_run(%s, %s); /* escaping join */", ce->hjoins[i].frame, sv);
            else if (ce->ret_mode)
                ce_line(ce, "return %s; /* escaping join via downstream */", sv);
            else
                ce_line(ce, "return dk_run(__kont, %s); /* escaping join via downstream */", sv);
            free(sv);
            return;
        }
        /* KK_VAR: an inline join */
        ce_line(ce, "%s = %s;", join_param(ce, kont->id), v);
        ce_line(ce, "goto %sL%u;", ce->lbl_pfx ? ce->lbl_pfx : "", kont->id);
    }
}

/* Deliver with no explicit value Type (the crossing type comes from the
 * continuation target -- ret_ty / cur_ty). */
static void emit_deliver(CE *ce, const CKont *kont, const char *v) {
    emit_deliver_ty(ce, kont, v, NULL);
}

static void emit_println(CE *ce, const BuiltinSpec *sp, const char *arg) {
    char *stmt = builtin_print_stmt(sp, sp->shape, arg);
    ce_line(ce, "%s", stmt);
    free(stmt);
}

/* ---------------------------------------------------------------------------
 * cps-edge-walk-misses-nodes-and-colored-frames-leak: the CPS ownership gap.
 *
 * The direct emitter frees a fresh sum-carrier box (`(ok 1)`), an owned `any`,
 * or a boxed value-struct payload that is handed to a NON-RETAINING callee by
 * queueing the temp at the argument hoist (sum_pending / any_pending /
 * vsp_pending) and draining the queue after the consuming call materialises.
 * That drain lives in emit_value's call hoist.  On the CPS path the argument
 * is lowered to its own CT_LETRAW -- delegated to emit_value, so the queue
 * push still happens -- but the consuming call is a CT_LETCALL / CT_TAILCALL
 * the CPS emitter writes itself, and no CPS statement ever drained the queue:
 * the entry sat in `ctx->sum_pending` for the rest of the program and the box
 * leaked (16 bytes per `(ok? (result-map (ok 1) f))` in a colored function,
 * typed/result-basic under tests/run-leak-check.sh).
 *
 * The mechanism here is the same discipline at the CPS statement boundary.
 * emit_letraw takes the queue marks around its delegation and moves anything
 * pushed into this table, keyed by the binder the delegated value lands in;
 * every CPS consumer of an atom (letcall, letprim, the cps->direct tail arm)
 * fires the matching entry's drop right after its call statement, spelled on
 * the ATOM (the binder is in scope in the consuming segment; the direct temp
 * may not be, once a continuation is lifted).  An entry never consumed is a
 * status-quo leak, never a free; the table is cleared per emitted function so
 * a binder id can never match across functions.  A cps->cps tail call has no
 * statement after `return f__cps(...)`; when one of its atoms has an entry the
 * arm binds the answer, fires the drop and returns it (see the tail arm) --
 * sound because `f__cps` runs the callee and the whole continuation before
 * returning, and the callee was proven non-retaining. */
typedef struct {
    uint32_t       cvar_id;
    const Binding *bind;
    int            kind;     /* 0 sum box, 1 any, 2 boxed value-struct payload */
    Type           t;
    bool           owned;    /* t is a malloc'd spine (sum_pending_owned) */
} CpsDeferredDrop;
static CpsDeferredDrop g_cps_deferred[256];
static uint32_t        g_n_cps_deferred;

static void cps_deferred_reset(void) {
    for (uint32_t i = 0; i < g_n_cps_deferred; i++)
        if (g_cps_deferred[i].owned) free_struct_app_type(g_cps_deferred[i].t);
    g_n_cps_deferred = 0;
}

static void cps_deferred_capture(CE *ce, const CTerm *t,
                                 uint32_t sum_mark, uint32_t any_mark, uint32_t vsp_mark) {
    EmitCtx *ctx = ce->ctx;
    while (ctx->n_sum_pending > sum_mark) {
        uint32_t k = --ctx->n_sum_pending;
        free(ctx->sum_pending[k]);
        if (g_n_cps_deferred < 256) {
            CpsDeferredDrop *d = &g_cps_deferred[g_n_cps_deferred++];
            d->cvar_id = t->as.letraw.x.id; d->bind = t->as.letraw.x.bind;
            d->kind = 0; d->t = ctx->sum_pending_types[k]; d->owned = ctx->sum_pending_owned[k];
        } else if (ctx->sum_pending_owned[k]) {
            free_struct_app_type(ctx->sum_pending_types[k]);
        }
    }
    while (ctx->n_any_pending > any_mark) {
        uint32_t k = --ctx->n_any_pending;
        free(ctx->any_pending[k]);
        if (g_n_cps_deferred < 256) {
            CpsDeferredDrop *d = &g_cps_deferred[g_n_cps_deferred++];
            d->cvar_id = t->as.letraw.x.id; d->bind = t->as.letraw.x.bind;
            d->kind = 1; memset(&d->t, 0, sizeof d->t); d->owned = false;
        }
    }
    while (ctx->n_vsp_pending > vsp_mark) {
        uint32_t k = --ctx->n_vsp_pending;
        free(ctx->vsp_pending[k]);
        if (g_n_cps_deferred < 256) {
            CpsDeferredDrop *d = &g_cps_deferred[g_n_cps_deferred++];
            d->cvar_id = t->as.letraw.x.id; d->bind = t->as.letraw.x.bind;
            d->kind = 2; d->t = ctx->vsp_pending_types[k]; d->owned = false;
        }
    }
}

/* The producer twin of cps_deferred_capture: a call the CPS emitter writes
 * ITSELF (a cps->direct letcall or tail call), so there is no emit_value hoist
 * to queue its result.  When elab stamped it for the drop-after-reader free
 * (`(ok? (result-map (ok 1) f))`: `result-map` returns a fresh carrier box and
 * `ok?` keeps nothing) and it really returns the carrier word, register the
 * drop on the binder the value lands in; the reader's consume fires it.  A
 * by-value result is boxed and reaped at delivery instead, so it is never
 * registered.  The leak this closes surfaced once that call stopped resolving,
 * by accident, to a spec another call in the unit had minted at the carrier
 * argument (typed/result-basic under tests/run-leak-check.sh). */
static void cps_deferred_note_producer(CE *ce, const Expr *call, const char *ret_cty,
                                       uint32_t cvar_id, const Binding *bind) {
    if (!call || !ret_cty || strcmp(ret_cty, "int64_t") != 0) return;
    if (!emit_call_owes_sum_drop(ce->ctx, call)) return;
    if (g_n_cps_deferred >= 256) return;
    CpsDeferredDrop *d = &g_cps_deferred[g_n_cps_deferred++];
    d->cvar_id = cvar_id; d->bind = bind;
    d->kind = 0; d->t = call->type; d->owned = false;
}

static bool cps_deferred_any_atom(const CAtom *args, uint32_t n) {
    if (g_n_cps_deferred == 0 || !args) return false;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 0; k < g_n_cps_deferred; k++) {
            const CpsDeferredDrop *d = &g_cps_deferred[k];
            if ((args[i].kind == CA_CVAR && args[i].cvar_id == d->cvar_id) ||
                (args[i].kind == CA_VAR && d->bind && args[i].var == d->bind))
                return true;
        }
    return false;
}

static void cps_deferred_consume_atoms(CE *ce, const CAtom *args, uint32_t n) {
    if (g_n_cps_deferred == 0 || !args) return;
    for (uint32_t i = 0; i < n; i++) {
        const CAtom *a = &args[i];
        for (uint32_t k = 0; k < g_n_cps_deferred; k++) {
            CpsDeferredDrop *d = &g_cps_deferred[k];
            bool hit = (a->kind == CA_CVAR && a->cvar_id == d->cvar_id) ||
                       (a->kind == CA_VAR && d->bind && a->var == d->bind);
            if (!hit) continue;
            char *v = atom_str(ce, a);
            Buf nb; buf_init(&nb);
            buf_printf(&nb, "(%s)", v);
            buf_putc(&nb, '\0');
            int saved = ce->ctx->indent;
            ce->ctx->indent = ce->indent;
            emit_pending_drop_stmt(ce->ctx, ce->out, d->kind, nb.data, d->t);
            ce->ctx->indent = saved;
            buf_free(&nb); free(v);
            if (d->owned) free_struct_app_type(d->t);
            g_cps_deferred[k] = g_cps_deferred[--g_n_cps_deferred];
            break;
        }
    }
}

static void emit_term(CE *ce, const CTerm *t) {
    switch (t->kind) {
        case CT_APPCONT: {
            char *v = atom_str(ce, &t->as.appcont.v);
            /* The delivered atom carries its own Type -- the most precise source
             * for Tier C boxing (a cross-function shift delivers to a prompt whose
             * result type the enclosing cur_ty does not know). */
            emit_deliver_ty(ce, &t->as.appcont.kont, v, t->as.appcont.v.type);
            free(v);
            break;
        }
        case CT_LETVAL: {
            char *v = atom_str(ce, &t->as.letval.v);
            char *bn = cvar_cname(ce, t->as.letval.x);
            if (is_byref_mut(t->as.letval.x.bind)) {
                /* B7: allocate the shared heap cell and store the initial value.
                 * The cell is leaked with the DK nodes (reaped at the outermost
                 * entry boundary) -- a stored continuation may still be resumed
                 * after this fn's dynamic extent, so it cannot be freed here. */
                const Binding *cb = t->as.letval.x.bind;
                ce_line(ce, "%s = (%s)malloc(sizeof(%s)); *%s = %s;",
                        bn, byref_cell_ptr_ctype(ce->ctx, cb),
                        byref_cell_ctype(ce->ctx, cb), bn, v);
                ce_line(ce, "__dk_reap_ptr((intptr_t)%s);", bn);
            } else {
                ce_line(ce, "%s = %s;", bn, v);
            }
            free(bn); free(v);
            emit_term(ce, t->as.letval.body);
            break;
        }
        case CT_LETPRIM: {
            uint32_t n = t->as.letprim.n;
            char **as = (char **)calloc(n ? n : 1, sizeof(char *));
            for (uint32_t i = 0; i < n; i++) as[i] = atom_str(ce, &t->as.letprim.args[i]);
            const BuiltinSpec *sp = t->as.letprim.spec;
            char *bn = cvar_cname(ce, t->as.letprim.x);
            if (sp->c_op && strcmp(sp->c_op, "TUR_DK_CONT_PRED") == 0) {
                /* (cont? k): the DK handler-case continuation is unconsumed until
                 * a user `resume` sets its `consumed` flag (emit_resume).  The k
                 * atom is `int64_t` or `DK *` depending on the case; `(DK *)(intptr_t)`
                 * normalizes both. */
                ce_line(ce, "%s = !((DK *)(intptr_t)(%s))->consumed;", bn, n ? as[0] : "0");
            } else if (is_println_shape(sp->shape)) {
                emit_println(ce, sp, n ? as[0] : "0");
                ce_line(ce, "%s = 0;", bn);  /* nil result */
            } else {
                char *rhs = prim_expr(sp, as, n);
                ce_line(ce, "%s = %s;", bn, rhs);
                free(rhs);
            }
            free(bn);
            for (uint32_t i = 0; i < n; i++) free(as[i]);
            free(as);
            cps_deferred_consume_atoms(ce, t->as.letprim.args, n);
            emit_term(ce, t->as.letprim.body);
            break;
        }
        case CT_LETCALL: {
            /* cps->direct: an uncolored callee runs to completion and returns an
             * ordinary value; no continuation is threaded in.  Resolve a
             * monomorph-clone name the same way the CT_TAILCALL cps->direct arm
             * does: a generic constructor/callee (e.g. the `tcons` built inside a
             * generated `_un_uncons_hyfmap` hylo helper) is emitted as the DEFINED
             * `<name>__spec__...` clone, not the unmangled generic symbol -- which
             * would otherwise be an undefined reference surviving only by -O2 DCE
             * of the dead helper.  find_mono_clone_for_call returns NULL for an
             * ordinary callee with no registered ABI specialization, so this is a
             * no-op for every non-templated call. */
            /* RC2 (generic-show-wrapper-cps-monomorphization-plan): re-resolve a
             * carrier-erased typeclass-method dispatch to the concrete per-spec
             * instance (same as the CT_TAILCALL arm); self-gating, so no-op for an
             * ordinary call. */
            char *rr_lc = t->as.letcall.call_expr
                ? emit_reresolve_method_call(ce->ctx, t->as.letcall.call_expr) : NULL;
            const char *mclone_lc = rr_lc ? NULL : find_mono_clone_for_call(
                ce->ctx, t->as.letcall.fn, t->as.letcall.args, t->as.letcall.n,
                t->as.letcall.call_expr);
            char *fn = rr_lc ? rr_lc
                     : (mclone_lc ? strdup(mclone_lc) : callee_name(t->as.letcall.fn));
            /* Cast each arg to the (direct) callee's declared param C type --
             * gcc14-int-conversion, carrier-to-typed-param: a cps->direct call to
             * e.g. `option_hyeq_qu(int64_t,int64_t,int64_t)` with a void* closure
             * arg 3 must carrier-cast it.  Skipped for a mono-clone (its args are
             * already the clone's concrete param types). */
            /* rr_lc/mclone_lc callees take the concrete element type by its own C
             * repr (the spec-clone param already has that repr), so pass raw --
             * casting to the ORIGINAL carrier-rep callee's int64 param would
             * mismatch the re-resolved `const char *` / concrete param. */
            const EmitAbiSpecialization *lc_spec =
                find_spec_by_clone_name(ce->ctx, fn);
            char *argv = (rr_lc || mclone_lc)
                ? atoms_csv_call_clone(ce, t->as.letcall.args, t->as.letcall.n, fn)
                : atoms_csv_call_typed(ce, t->as.letcall.args, t->as.letcall.n,
                                       t->as.letcall.fn, lc_spec);
            char *bn = cvar_cname(ce, t->as.letcall.x);
            /* RM3 R4 (docs/archive/regions-plan.md): NO region boundary here,
             * deliberately.  The CPS path's `bt-scope` calls all land on the
             * CT_TAILCALL cps->direct arm, which carries the push/pop -- probed
             * with the bracket in tail position, in an arithmetic operand, and
             * bound twice in a `let`, and every one of them lowered to a tailcall
             * plus a letcont.  A copy of the mechanism here would be a second,
             * untested transcription of the two-lock rule, which is worse than a
             * missed saving.  If a shape ever does reach this arm with a
             * `bt-scope` callee it costs no correctness -- an unbracketed call is
             * exactly a flag-off build -- and the fix is to mirror the tailcall
             * arm's block, predicates and all. */
            /* A `:nil`/`:void`-returning callee (e.g. `tur_contract_check`) yields
             * no value: emit the call as a bare statement and bind the unit
             * placeholder, never `x = void_fn(...)` (a C "void value not ignored"
             * error).  Mirrors emit_letraw's nil handling; needed once a nil
             * cps->direct call rides a lifted join/frame body (P6). */
            if (t->as.letcall.x.ty == TY_NIL) {
                ce_line(ce, "%s(%s); /* cps->direct (nil) */", fn, argv);
                ce_line(ce, "%s = 0;", bn);
            } else if (mclone_lc) {
                /* A resolved mono-clone returns its real type -- for a constructor
                 * clone (`tcons__spec__...`) that is a boxed-ADT pointer -- while
                 * the binder slot's C type may be int64_t OR a concrete pointer
                 * (`tur_adt_Map__String__int *`, when the letcall's own Type is a
                 * nominal parametric-struct).  Bridge to the BINDER's declared C
                 * type, not a hardcoded int64_t: casting a pointer-returning clone
                 * to int64_t and assigning into a pointer binder is a
                 * -Wint-conversion straddle (macOS clang treats it as a hard error).
                 * (int64_t)(intptr_t) / (P *)(intptr_t) both round-trip the word,
                 * so this is value-preserving in either direction. An aggregate
                 * binder (neither pointer nor int64) cannot ride an intptr_t cast --
                 * fall back to a raw assignment there. */
                const char *bct = binder_ctype_full(ce->ctx, t->as.letcall.x.ty,
                                                    t->as.letcall.x.type);
                size_t bL = bct ? strlen(bct) : 0;
                bool bct_bridgeable = bct && (strcmp(bct, "int64_t") == 0 ||
                                              (bL >= 1 && bct[bL - 1] == '*'));
                /* cps-direct-mono-clone-aggregate-result: the resolved clone
                 * of a generic whose result is a by-value aggregate (`id` at
                 * `A := B1`) returns the struct, and an int64 binder cannot
                 * take it through intptr_t -- invalid C (found by the type
                 * fuzzer's gbody crossing).  Pack it the way emit_letraw
                 * does: a reaped heap box, and a float's bits. */
                const char *rct_mc = emit_sig_lookup_ret_ctype(fn);
                bool bct_i64 = bct && strcmp(bct, "int64_t") == 0;
                if (bct_i64 && rct_mc && strncmp(rct_mc, "tur_adt_", 8) == 0 &&
                    !strchr(rct_mc, '*'))
                    ce_line(ce, "%s = __dk_reap_ptr((intptr_t)({ %s *__bx = (%s *)malloc(sizeof(%s)); *__bx = %s(%s); __bx; })); /* cps->direct */",
                            bn, rct_mc, rct_mc, rct_mc, fn, argv);
                else if (bct_i64 && rct_mc &&
                         (strcmp(rct_mc, "double") == 0 || strcmp(rct_mc, "float") == 0)) {
                    Buf cb; buf_init(&cb);
                    buf_printf(&cb, "%s(%s)", fn, argv);
                    buf_putc(&cb, '\0');
                    char *br = emit_carrier_bridge(ce->ctx, ce->out, strdup(cb.data),
                        CK_CONCRETE, CK_CARRIER,
                        emit_type_from_kind(strcmp(rct_mc, "float") == 0 ? TY_FLOAT32
                                                                         : TY_FLOAT));
                    buf_free(&cb);
                    ce_line(ce, "%s = %s; /* cps->direct */", bn, br);
                    free(br);
                } else if (bct_bridgeable)
                    ce_line(ce, "%s = (%s)(intptr_t)%s(%s); /* cps->direct */", bn, bct, fn, argv);
                else
                    ce_line(ce, "%s = %s(%s); /* cps->direct */", bn, fn, argv);
            } else {
                /* saffron-cps-vec-element-carrier-mismatch: the CPS twin of the
                 * `temp_is_tagged` bridge in emit_expr.c's
                 * bridge_control_result_int_ptr (saffron-lang-plan S6).
                 *
                 * An `any` read out of a container arrives as the SLOT WORD --
                 * the store side boxed it, so the word is a `tur_tagged_t *`
                 * -- while the binder here is the aggregate.  The direct
                 * emitter dereferences; this arm assigned raw, which is
                 * "incompatible types when assigning to type 'tur_tagged_t'
                 * from type 'int64_t'": a hard error, so a Saffron function
                 * that both performed and read a vector element did not build.
                 *
                 * Keyed on both sides like its direct twin -- the binder must
                 * be the aggregate AND the callee must really return the
                 * carrier, per the signature side table -- so a callee that
                 * already returns a `tur_tagged_t` is untouched. */
                const char *bct_lc = binder_ctype_full(ce->ctx, t->as.letcall.x.ty,
                                                       t->as.letcall.x.type);
                const char *rct_lc = emit_sig_lookup_ret_ctype(fn);
                if (bct_lc && strcmp(bct_lc, "tur_tagged_t") == 0 &&
                    rct_lc && strcmp(rct_lc, "int64_t") == 0)
                    ce_line(ce, "%s = *(tur_tagged_t *)(intptr_t)%s(%s); /* cps->direct (any elem) */",
                            bn, fn, argv);
                else
                    ce_line(ce, "%s = %s(%s); /* cps->direct */", bn, fn, argv);
            }
            /* A by-value clone result is boxed and reaped above, so only the
             * unresolved callee's carrier word can owe the reader's drop. */
            if (!mclone_lc && !rr_lc && t->as.letcall.x.ty != TY_NIL)
                cps_deferred_note_producer(ce, t->as.letcall.call_expr,
                                           emit_sig_lookup_ret_ctype(fn),
                                           t->as.letcall.x.id, t->as.letcall.x.bind);
            free(bn); free(fn); free(argv);
            /* cps-body-panic-not-propagated: a cps->direct callee that panicked
             * under a handler signals by return; propagate before running the
             * rest of this body (and the continuation). */
            cps_panic_check(ce);
            cps_deferred_consume_atoms(ce, t->as.letcall.args, t->as.letcall.n);
            emit_term(ce, t->as.letcall.body);
            break;
        }
        case CT_TAILCALL: {
            if (t->as.tailcall.via_fncps) {
                /* E2 (fat-closure fn-value threading): the callee is a poly-fn
                 * PARAM whose value is a `tur_poly_fn_t` fat closure.  When the
                 * passed fn-value is EFFECTFUL its `fn_cps` DK-threading slot is
                 * populated -- tail-dispatch through it, threading the current
                 * continuation so the callback's `perform` reaches the caller's
                 * handler.  When the slot is NULL (a pure fn-value) fall to the
                 * direct `f.fn` call, delivering its result to the continuation
                 * exactly as the delegated CT_LETRAW path did. */
                char *pf = name_for_binding(ce->ctx, t->as.tailcall.fn);
                uint32_t na = t->as.tailcall.n;
                char **av = fncps_args_str(ce, t->as.tailcall.args, na);
                const char *thread = (t->as.tailcall.kont.kind == KK_PROMPT)
                    ? (ce->cur_k ? ce->cur_k : "__kont") : "__kont";
                Buf sc; buf_init(&sc);
                fncps_slot_call(&sc, pf, av, na, thread);
                buf_putc(&sc, '\0');
                ce_line(ce, "if (%s.fn_cps) return %s; /* E2 threaded fat fn-value */",
                        pf, sc.data);
                buf_free(&sc);
                /* Pure fallback: call the direct entry and deliver the result. */
                Buf pv; buf_init(&pv);
                fncps_direct_call(&pv, pf, av, na, t->as.tailcall.call_expr);
                buf_putc(&pv, '\0');
                emit_deliver(ce, &t->as.tailcall.kont, pv.data);
                buf_free(&pv);
                free(pf); fncps_args_free(av, na);
                break;
            }
            if (t->as.tailcall.via_registry) {
                /* E2a: callee is a fn-value PARAM; recover its CPS entry from the
                 * registry and thread __kont so its perform reaches the caller's
                 * handler.  __cps ABI: int64_t (*)(int64_t args..., DK*).  E2c: when
                 * fn == NULL the callee is a struct-field fn-value LOAD carried in
                 * fn_atom -- use its atom expression as the lookup key. */
                char *pf = t->as.tailcall.fn ? callee_name(t->as.tailcall.fn)
                                             : atom_str(ce, &t->as.tailcall.fn_atom);
                /* E2a carrier ABI: every arg slot is int64_t, so pointer-like args
                 * must be carrier-cast (gcc14-int-conversion). */
                char *argv = atoms_csv_call_cps(ce, t->as.tailcall.args, t->as.tailcall.n);
                const char *thread = (t->as.tailcall.kont.kind == KK_PROMPT)
                    ? (ce->cur_k ? ce->cur_k : "__kont") : "__kont";
                const Type *e2sig = e2a_callee_sig(t->as.tailcall.fn,
                                                   &t->as.tailcall.fn_atom,
                                                   t->as.tailcall.n);
                if (e2a_call_is_fat(t->as.tailcall.fn, &t->as.tailcall.fn_atom)) {
                    emit_e2a_fat_dispatch(ce, pf, pf, argv, t->as.tailcall.n,
                                          thread, "E2a threaded fn-value", e2sig);
                    free(pf); free(argv);
                    break;
                }
                /* cast to the __cps ABI, float parameters at their own type */
                char cast[512];
                e2a_cast(cast, sizeof cast, "", e2sig, t->as.tailcall.n);
                char key[640];
                e2a_lookup_key(key, sizeof key, t->as.tailcall.fn, pf);
                if (t->as.tailcall.n)
                    ce_line(ce, "return ((%s)__tur_cps_lookup_checked(%s, \"%s\"))(%s, %s); /* E2a threaded fn-value */",
                            cast, key, pf, argv, thread);
                else
                    ce_line(ce, "return ((%s)__tur_cps_lookup_checked(%s, \"%s\"))(%s); /* E2a threaded fn-value */",
                            cast, key, pf, thread);
                free(pf); free(argv);
                break;
            }
            /* G3b: a colored-generic mono-template callee -- resolve this call
             * site to the monomorph clone so we thread `<clone>__cps` (the DK
             * monomorph), not the unresolved generic name.
             *
             * RC1 (generic-show-wrapper-cps-monomorphization-plan): an UNCOLORED
             * generic callee -- e.g. the `^Show a` wrapper `show-line`/`print-show`
             * -- is not a colored mono_template, so the base gate leaves the clone
             * unresolved and the call falls to the generic BASE clone, whose body
             * bakes the int carrier representative (`__inst_Show_show_int`) and
             * misrenders every non-int element type. Its per-element DIRECT clone IS
             * registered as an ABI spec; resolve it and route the call through the
             * cps->direct path. The clone is direct-only (no `<clone>__cps` variant
             * is emitted for an uncolored callee), so it must NOT take the cps->cps
             * branch (which would be an undefined reference).
             *
             * For a NOMINAL element type (String/Vec/Set/Map) the resolved direct
             * clone's body dispatches correctly. A carrier-SCALAR element
             * (cstr/bool/...) resolves to a clone whose body is itself COLORED, so
             * its inner `(show x)` is emitted here (not by the direct emitter) with
             * the baked carrier rep -- RC2 below re-resolves that. */
            /* RC2 (generic-show-wrapper-cps-monomorphization-plan): a carrier-erased
             * typeclass-method dispatch (`(show x)` baked to `__inst_Show_show_int`)
             * inside a COLORED ABI-spec clone body reaches the CPS emitter, which
             * would otherwise call the int carrier rep verbatim.  Run the same
             * per-spec re-resolution the direct emitter uses (emit_reresolve_method_call)
             * so the body dispatches to `__inst_Show_show_<T>` for the spec's concrete
             * element T.  It self-gates: NULL unless the call is a genuine tyvar
             * dispatch inside an active spec, so an ordinary int dispatch is
             * untouched.  The re-resolved instance method is a DIRECT (uncolored)
             * callee, so force the cps->direct path. */
            char *rr = t->as.tailcall.call_expr
                ? emit_reresolve_method_call(ce->ctx, t->as.tailcall.call_expr) : NULL;
            const char *clone = NULL;
            bool clone_is_cps = false;   /* true only when <clone>__cps is emitted */
            SEnt *fe = ent_of_binding(t->as.tailcall.fn);
            bool callee_colored = binding_in_s(t->as.tailcall.fn);
            if (!rr && fe && fe->mono_template) {
                clone = find_mono_clone_for_call(ce->ctx, t->as.tailcall.fn,
                                                 t->as.tailcall.args, t->as.tailcall.n,
                                                 t->as.tailcall.call_expr);
                clone_is_cps = (clone != NULL);  /* colored mono-template -> <clone>__cps */
            } else if (!rr && fe && !callee_colored) {
                clone = find_mono_clone_for_call(ce->ctx, t->as.tailcall.fn,
                                                 t->as.tailcall.args, t->as.tailcall.n,
                                                 t->as.tailcall.call_expr);
                /* uncolored generic clone is direct-only: clone_is_cps stays false */
            }
            char *fn = rr ? rr : (clone ? strdup(clone) : callee_name(t->as.tailcall.fn));
            char *argv = atoms_csv_call(ce, t->as.tailcall.args, t->as.tailcall.n);
            /* region-bracket-lost-when-bt-scope-specializes: a region boundary
             * has to take the cps->direct arm, because that is the only arm the
             * bracket can live on.  `cps->cps` is a TAIL call -- the callee
             * delivers to `__kont` itself and this frame never runs again -- so
             * there is nowhere to put the pop; wrapping `__kont` in a
             * region-popping DK frame is the alternative, and it is a great deal
             * of machinery for a boundary that is already direct everywhere else.
             *
             * "Everywhere else" is the point: `bt-scope`'s base is colored but
             * fell back to DIRECT style (no `bt_hyscope__cps` is ever emitted),
             * so every shipping call site already lands on cps->direct.  Only a
             * resolved SPEC clone reached cps->cps, and only because a colored
             * mono-template emits `<clone>__cps` even when its own base did not.
             * Forcing direct here makes a `bt-scope` at an aggregate result
             * behave exactly like the scalar one every existing fixture covers,
             * rather than introducing a third behaviour.
             *
             * Scoped to the one callee `emit_binding_is_region_scope` names, and
             * that predicate is false unless `--enable=regions` is on, so no
             * default-path routing moves.  The residual asymmetry -- a colored
             * mono-template emitting CPS specs of a direct-style base -- is not
             * fixed here; it is the general shape this is a local answer to. */
            bool force_direct = emit_binding_is_region_scope(t->as.tailcall.fn);
            if (!rr && !force_direct && (callee_colored || clone_is_cps)) {
                /* cps->cps: both colored and emitted -- thread the continuation
                 * straight through, no trampoline.  The threaded continuation is
                 * the function's own k (KK_RET) or, inside a reset's delimited
                 * body, the enclosing prompt chain (KK_PROMPT).  A KK_VAR here
                 * would need a heap join, which excludes the caller. */
                const char *thread = (t->as.tailcall.kont.kind == KK_PROMPT)
                    ? ce->cur_k : "__kont";
                /* Cast each arg to the callee's DECLARED param C type (int64, a
                 * void pointer, or a concrete pointer) -- gcc14-int-conversion,
                 * carrier-to-typed-param. */
                size_t *arg_offs = (size_t *)malloc((t->as.tailcall.n ? t->as.tailcall.n : 1) * sizeof(size_t));
                char *argv_t = atoms_csv_call_typed_offs(ce, t->as.tailcall.args,
                                                         t->as.tailcall.n, t->as.tailcall.fn,
                                                         find_spec_by_clone_name(ce->ctx, fn),
                                                         arg_offs);
                /* cps-self-tail-call-relies-on-sibling-call: a self tail call
                 * that hands on the function's own continuation, in its main
                 * body, is a loop.  `return f__cps(args, __kont)` left it to the
                 * C compiler's sibling-call optimization -- gcc makes it at -O2,
                 * not at -O1 (every ASan build), and a million-element Scheme
                 * `for-each` overflowed there.  Rebind the parameters (all
                 * arguments are evaluated first: they may read the old ones)
                 * and jump to the top of the body, the backedge the direct
                 * emitter's self-TCO already gives a non-CPS function.
                 * `__kont` is unchanged across it, so it needs no rebinding. */
                /* cps-self-tail-call-relies-on-sibling-call (mutual
                 * recursion): a tail call from a fused group member's main
                 * body to any member -- itself included -- that hands on
                 * `__kont` is a jump inside the group's function.  Same order
                 * as the backedge below: every argument into a temp first
                 * (they may read this member's parameters), then the
                 * target's slots, then the dispatch. */
                if (ce->tcg && ce->out == ce->tb_out && !rr && !clone &&
                    t->as.tailcall.kont.kind == KK_RET &&
                    !ce->shift_mode && !ce->ret_mode && !ce->handler_case_mode &&
                    !cps_deferred_any_atom(t->as.tailcall.args, t->as.tailcall.n)) {
                    const FnDef *tfd = g_prog ? fd_for_binding(g_prog, t->as.tailcall.fn)
                                              : NULL;
                    int tj = ctg_member_idx(ce->tcg, tfd);
                    if (tj >= 0 && t->as.tailcall.n == tfd->n_params) {
                        uint32_t base = ce->tcg->slot_base[tj];
                        ce_line(ce, "{   /* cps->cps group tail call: a jump */");
                        for (uint32_t i = 0; i < t->as.tailcall.n; i++) {
                            size_t from = arg_offs[i];
                            size_t to = (i + 1 < t->as.tailcall.n) ? arg_offs[i + 1] - 2
                                                                   : strlen(argv_t);
                            ce_line(ce, "    %s __tb%u = %.*s;",
                                    emit_param_ctype(ce->ctx, tfd, i), i,
                                    (int)(to - from), argv_t + from);
                        }
                        for (uint32_t i = 0; i < t->as.tailcall.n; i++)
                            ce_line(ce, "    __tcg_s%u = __tb%u;", (unsigned)(base + i), i);
                        ce_line(ce, "    __tcg_st = %d;", tj);
                        ce_line(ce, "    goto __tcg_top;");
                        ce_line(ce, "}");
                        free(argv_t);
                        free(arg_offs);
                        free(fn);
                        free(argv);
                        break;
                    }
                }
                const FnDef *sfd = ce->self_fd;
                bool self_loop = sfd && ce->out == ce->self_out && !rr && !clone &&
                    t->as.tailcall.fn == sfd->binding && t->as.tailcall.kont.kind == KK_RET &&
                    !ce->shift_mode && !ce->ret_mode && !ce->handler_case_mode &&
                    t->as.tailcall.n == sfd->n_params &&
                    !cps_deferred_any_atom(t->as.tailcall.args, t->as.tailcall.n);
                if (self_loop) {
                    /* The temps are block-scoped, so their names need not be
                     * unique in the function (and take no emitter counter). */
                    ce_line(ce, "{   /* cps->cps self tail call: a backedge */");
                    for (uint32_t i = 0; i < t->as.tailcall.n; i++) {
                        size_t from = arg_offs[i];
                        size_t to = (i + 1 < t->as.tailcall.n) ? arg_offs[i + 1] - 2 : strlen(argv_t);
                        ce_line(ce, "    %s __tb%u = %.*s;", emit_param_ctype(ce->ctx, sfd, i),
                                i, (int)(to - from), argv_t + from);
                    }
                    /* The arguments are computed (they may call a closure
                     * below); the closures this turn bound are dead now. */
                    for (uint32_t i = 0; i < ce->n_loop_reaps; i++)
                        ce_line(ce, "    __dk_reap_closure_now((intptr_t)%s);",
                                ce->loop_reaps[i]);
                    for (uint32_t i = 0; i < t->as.tailcall.n; i++) {
                        char *pn = name_for_binding(ce->ctx, sfd->params[i]);
                        ce_line(ce, "    %s = __tb%u;", pn, i);
                        free(pn);
                    }
                    ce_line(ce, "    goto __tur_cps_self;");
                    ce_line(ce, "}");
                    *ce->self_backedge = true;
                    free(argv_t);
                    free(arg_offs);
                    free(fn);
                    free(argv);
                    break;
                }
                free(arg_offs);
                /* cps-edge-walk-misses-nodes-and-colored-frames-leak, the tail
                 * arm: a fresh box handed to this callee (a deferred drop keyed
                 * on one of the arg atoms) has nothing after `return f__cps(...)`
                 * to free it.  `f__cps` runs the callee AND delivers its result
                 * to `__kont` -- the rest of the computation -- before
                 * returning, and the callee was proven non-retaining at elab,
                 * so once the call returns the box is dead: bind the answer,
                 * fire the drop, return it.  The cost is one lost C tail call
                 * for exactly this shape; the lifetime is the precise one, not
                 * the driver-boundary reap.  No deferred entry: the tail call is
                 * emitted as before. */
                if (cps_deferred_any_atom(t->as.tailcall.args, t->as.tailcall.n)) {
                    char *rv = fresh_tmp(ce->ctx);
                    if (t->as.tailcall.n)
                        ce_line(ce, "int64_t %s = %s__cps(%s, %s); /* cps->cps, freed after */", rv, fn, argv_t, thread);
                    else
                        ce_line(ce, "int64_t %s = %s__cps(%s); /* cps->cps, freed after */", rv, fn, thread);
                    cps_deferred_consume_atoms(ce, t->as.tailcall.args, t->as.tailcall.n);
                    ce_line(ce, "return %s;", rv);
                    free(rv);
                } else if (t->as.tailcall.n)
                    ce_line(ce, "return %s__cps(%s, %s); /* cps->cps */", fn, argv_t, thread);
                else
                    ce_line(ce, "return %s__cps(%s); /* cps->cps */", fn, thread);
                free(argv_t);
            } else {
                /* cps->direct: the callee is uncolored or a colored function that
                 * fell back to direct-style; call it synchronously and deliver
                 * the ordinary result value to the continuation. */
                char *tmp = fresh_tmp(ce->ctx);
                /* Cast each arg to the callee's DECLARED param C type, exactly as
                 * the cps->cps branch does -- a pointer/fat-fn arg into an int64
                 * carrier param (or vice versa) is a -Wint-conversion hard error
                 * under GCC >= 14 (gcc14-int-conversion, carrier-to-typed-param).
                 * A re-resolved (rr) instance-method callee takes the concrete
                 * element by its own C repr (the spec-clone arg already has it), so
                 * pass raw -- casting to the original int carrier-rep param would
                 * mismatch. */
                char *argv_t = rr
                    ? atoms_csv_call(ce, t->as.tailcall.args, t->as.tailcall.n)
                    : atoms_csv_call_typed(ce, t->as.tailcall.args,
                                           t->as.tailcall.n, t->as.tailcall.fn,
                                           find_spec_by_clone_name(ce->ctx, fn));
                /* Edge 2a (generic-show-wrapper-cps-monomorphization-plan): a
                 * `:nil`/`:void`-returning callee (a `^Show a` wrapper like
                 * `show-line`/`print-show`, `tur_contract_check`, ...) emits as C
                 * `void`, so it yields no value: `__auto_type t = void_fn(...)` is a
                 * "variable declared void" hard error.  Emit a bare call and deliver
                 * the unit placeholder `0` -- mirrors the CT_LETCALL nil arm and
                 * emit_value's TY_NIL/TY_NEVER skip in the direct emitter.  Lands
                 * together with RC1 (direct-clone routing) + RC2 (colored-clone
                 * dispatch re-resolution) so no wrapper call silently misrenders. */
                const FnDef *cfd = g_prog
                    ? fd_for_binding(g_prog, t->as.tailcall.fn) : NULL;
                const Type *crt = cfd ? fn_ret_type(cfd) : NULL;
                /* RM3 R4: the region boundary on the CPS TAILCALL path.  This is
                 * the arm a `bt-scope` in a non-main defn actually lands on --
                 * the CT_LETCALL arm below covers the other CPS shape, and
                 * emit_value covers the direct one.  Three sites, one predicate
                 * pair (emit_binding_is_region_scope / emit_region_scope_reclaims),
                 * because a second copy of the static walk would drift.
                 *
                 * The instantiated result type comes from `call_expr`, not from
                 * the callee's declared return: `bt-scope [A] ... : A` declares a
                 * TYPE VARIABLE, which the walk would (correctly, and uselessly)
                 * refuse every time. */
                int  rgn_id = -1;
                bool rgn_reclaim = false;
                if (emit_binding_is_region_scope(t->as.tailcall.fn) &&
                    crt && crt->kind != TY_NIL && crt->kind != TY_NEVER) {
                    const Expr *rce = t->as.tailcall.call_expr;
                    rgn_reclaim = rce ? emit_region_scope_reclaims(ce->ctx, &rce->type)
                                      : false;
                    rgn_id = (int)ce->ctx->tmp_n++;
                    ce_line(ce, "int __tur_rgn_%d = tur_region_push();", rgn_id);
                }
                if (crt && (crt->kind == TY_NIL || crt->kind == TY_NEVER)) {
                    ce_line(ce, "%s(%s); /* cps->direct (nil) */", fn, argv_t);
                    cps_panic_check(ce);   /* cps-body-panic-not-propagated */
                    cps_deferred_consume_atoms(ce, t->as.tailcall.args, t->as.tailcall.n);
                    emit_deliver(ce, &t->as.tailcall.kont, "0");
                } else {
                    /* S1/findings 16: name the callee's real return type from the
                     * signature side table (the forward declaration as actually
                     * emitted) so the temp is c2mir-clean; __auto_type only when
                     * the record is missing.  The slot cast at delivery narrows
                     * it to the word either way. */
                    const char *drt = emit_sig_lookup_ret_ctype(fn);
                    if (drt && *drt && strcmp(drt, "void") != 0)
                        ce_line(ce, "%s %s = %s(%s); /* cps->direct */", drt, tmp, fn, argv_t);
                    else
                        ce_line(ce, "__auto_type %s = %s(%s); /* cps->direct */", tmp, fn, argv_t);
                    /* region-escape-through-unhooked-stores item 3: the panic
                     * check returns before the pop below, stranding the
                     * generation; retire it on the panic arm first (as the
                     * direct path does). */
                    if (rgn_id >= 0)
                        ce_line(ce, "if (tur_panicking) TUR_REGION_RETIRE_TO(__tur_rgn_%d - 1);", rgn_id);
                    cps_panic_check(ce);   /* cps-body-panic-not-propagated */
                    cps_deferred_consume_atoms(ce, t->as.tailcall.args, t->as.tailcall.n);
                    /* RM3 R4: close the generation before the value is delivered
                     * to the continuation -- the continuation is the rest of the
                     * caller, so anything after this point is outside the bracket.
                     * Same two locks and same word-only note as the direct path. */
                    if (rgn_id >= 0) {
                        bool notable = drt && (strchr(drt, '*') != NULL ||
                                               strcmp(drt, "int64_t") == 0 ||
                                               strcmp(drt, "intptr_t") == 0);
                        if (rgn_reclaim && notable)
                            ce_line(ce, "tur_region_note_escape((const void *)(intptr_t)%s); "
                                        "(void)tur_region_pop_checked(__tur_rgn_%d);", tmp, rgn_id);
                        else if (rgn_reclaim)
                            /* region-lock-hardening: a by-value aggregate result
                             * is noted by its words -- an `:int` field can be an
                             * erased node.  Same rule as the direct path. */
                            ce_line(ce, "tur_region_note_escape_words(&%s, sizeof %s); "
                                        "(void)tur_region_pop_checked(__tur_rgn_%d);", tmp, tmp, rgn_id);
                        else
                            ce_line(ce, "tur_region_pop(__tur_rgn_%d);", rgn_id);
                        rgn_id = -1;   /* closed */
                    }
                    /* findings 28 (typed/result-basic): the callee resolved to a
                     * MONOMORPH/spec clone that returns its ADT BY VALUE
                     * (`tur_adt_Result__int__int`), while the IR types the call at
                     * the ERASED int64 carrier -- so the delivery slot is a word.
                     * Assigning the struct straight into it is "incompatible types
                     * when assigning" (gcc) / "incompatible types in assignment to
                     * an arithmetic type lvalue" (c2mir): a hard error on every
                     * COMPILING engine.  Bridge concrete->carrier the way the
                     * direct emitter does (spill, deliver the ADDRESS), heap-copied
                     * so the pointer outlives the delivering block and
                     * reap-registered so it is freed at the entry boundary.  The
                     * read side needs no change: every carrier consumer of an ADT
                     * word already derefs it (`tur_is_ok`, and the spec clone's own
                     * `(tur_adt_Result *)(intptr_t)r` parameter cast). */
                    const char *slot_cty = deliver_slot_cty(ce, &t->as.tailcall.kont);
                    /* A carrier word delivered to an inline join: the join
                     * parameter is the binder its reader consumes. */
                    if (t->as.tailcall.kont.kind == KK_VAR && !rr) {
                        for (int ji = ce->n_joins - 1; ji >= 0; ji--) {
                            if (ce->joins[ji].id != t->as.tailcall.kont.id) continue;
                            cps_deferred_note_producer(ce, t->as.tailcall.call_expr, drt,
                                                       ce->joins[ji].param_id,
                                                       ce->joins[ji].param_bind);
                            break;
                        }
                    }
                    if (cty_is_byval_agg(drt) && !cty_is_byval_agg(slot_cty)) {
                        Buf bx; buf_init(&bx);
                        buf_printf(&bx,
                            "(int64_t)__dk_reap_ptr((intptr_t)({ %s *__bx = "
                            "(%s *)malloc(sizeof(%s)); *__bx = %s; __bx; }))",
                            drt, drt, drt, tmp);
                        buf_putc(&bx, '\0');
                        emit_deliver(ce, &t->as.tailcall.kont, bx.data);
                        buf_free(&bx);
                    } else if (cty_word_straddle(drt, slot_cty)) {
                        /* An erasing ascription `(:: (f) :int)` over a callee
                         * that returns a concrete pointer (`tur_adt_Vec__int *`)
                         * lands the handle in an int64 join local (or the
                         * reverse).  A bare assignment is -Wint-conversion, a
                         * hard error on macOS clang; bridge through intptr_t. */
                        Buf bv; buf_init(&bv);
                        buf_printf(&bv, "(%s)(intptr_t)%s", slot_cty, tmp);
                        buf_putc(&bv, '\0');
                        emit_deliver(ce, &t->as.tailcall.kont, bv.data);
                        buf_free(&bv);
                    } else {
                        emit_deliver(ce, &t->as.tailcall.kont, tmp);
                    }
                }
                free(argv_t);
                free(tmp);
            }
            free(fn); free(argv);
            break;
        }
        case CT_LETCONT: {
            if (letcont_is_heap_join(t)) { emit_heap_join(ce, t); break; }
            if (letcont_is_escaping_join(t)) { emit_escaping_join(ce, t); break; }
            /* Name the join-param SLOT the way every reference to it is named
             * (cvar_cname -> name_for_binding when the param carries a source
             * Binding, so a kebab-case `let` binder like `first-results` mangles
             * to `first_hyresults_<id>` at the decl, the delivery, AND the join
             * body's uses).  Using the raw `param.name` here desynced the slot
             * (`first-results`, an invalid C identifier) from the body's mangled
             * atom references.  Must match emit_binder_decls' CT_LETCONT decl. */
            char *pn = cvar_cname(ce, t->as.letcont.param);
            bool pushed = ce->n_joins < MAX_JOINS;
            if (pushed) {
                ce->joins[ce->n_joins].id = t->as.letcont.j.id;
                ce->joins[ce->n_joins].param = pn;
                /* Same spelling emit_binder_decls declares the local with, so a
                 * delivery can compare the slot's C type against the delivered
                 * value's (the cps->direct aggregate bridge). */
                ce->joins[ce->n_joins].cty =
                    binder_ctype_full(ce->ctx, t->as.letcont.param.ty, t->as.letcont.param.type);
                ce->joins[ce->n_joins].param_id = t->as.letcont.param.id;
                ce->joins[ce->n_joins].param_bind = t->as.letcont.param.bind;
                ce->n_joins++;
            }
            emit_term(ce, t->as.letcont.body);
            if (pushed && ce->n_joins > 0) ce->n_joins--;
            /* the join landing pad */
            indent_buf(ce->out, ce->indent);
            buf_printf(ce->out, "%sL%u:;\n", ce->lbl_pfx ? ce->lbl_pfx : "",
                       t->as.letcont.j.id);
            emit_term(ce, t->as.letcont.jbody);
            free(pn);
            break;
        }
        case CT_IF: {
            char *c = atom_str(ce, &t->as.if_.cond);
            ce_line(ce, "if (%s) {", c);
            free(c);
            ce->indent += 4;
            emit_term(ce, t->as.if_.then_);
            ce->indent -= 4;
            ce_line(ce, "} else {");
            ce->indent += 4;
            emit_term(ce, t->as.if_.else_);
            ce->indent -= 4;
            ce_line(ce, "}");
            break;
        }
        case CT_MATCH:   emit_match(ce, t); break;
        case CT_RESET:   emit_reset(ce, t); break;
        case CT_SHIFT:   emit_shift(ce, t); break;
        case CT_HANDLE:  emit_handle(ce, t); break;
        case CT_PERFORM: emit_perform(ce, t); break;
        case CT_AWAIT:   emit_await(ce, t); break;
        case CT_RESUME:  emit_resume(ce, t); break;
        case CT_LETRAW:  emit_letraw(ce, t); break;
        case CT_CLONEABLE: emit_cloneable(ce, t); break;
        case CT_CALLCC:  emit_callcc(ce, t); break;
        case CT_LOOP:    emit_loop(ce, t); break;
        case CT_CONTINUE: emit_continue(ce, t); break;
        default:
            ce_line(ce, "abort(); /* cps-backend: unreachable */");
            break;
    }
}

/* B4: emit a restricted `match` on a heap-ADT carrier scrutinee (match_dk_ok).
 * The scrutinee is bound ONCE as a typed carrier pointer; each ctor arm tests
 * `->tag` in an if/else-if chain and reads its pattern fields inline via
 * adt_field_member_path (exactly as the direct emitter, emit_expr.c, does); the
 * arm body is emitted in the arm's block scope so its own captures (a perform
 * continuation, a nested join) ride the enclosing frame the same way a `let`
 * binder does.  A trailing catch-all (ctor == NULL) becomes the final `else`;
 * with no catch-all the (exhaustive, elaborator-guaranteed) fallthrough aborts. */
static void emit_match(CE *ce, const CTerm *t) {
    const AdtDef *adt = t->as.match.adt;
    char *scrut = atom_str(ce, &t->as.match.scrut);
    char *mn = mangle_adt_name(adt->name);
    char *sv = fresh_tmp(ce->ctx);
    /* SR1: a by-value sum scrutinee is an AGGREGATE, not a carrier pointer --
     * casting it through intptr_t trips "aggregate value used where an integer
     * was expected".  Bind a local copy and take its address, so every `->tag`
     * and `->as.<Ctor>._N` read below keeps working unchanged; the copy has
     * exactly this match's lifetime.  This mirrors the direct emitter's switch
     * path (emit_expr.c).  Keyed on the SCRUTINEE's own type kind, not the
     * pattern's ADT: a value that is statically a carrier word stays on the
     * pointer path even when its patterns name a by-value sum. */
    TypeKind sk = t->as.match.scrut.ty;
    bool scrut_carrier_word = (sk == TY_INT || sk == TY_INT64 ||
                               sk == TY_UINT64 || sk == TY_PTR_VOID);
    bool byval = adt_is_byvalue_product(adt) && !adt->is_heap &&
                 !scrut_carrier_word;
    if (byval) {
        char *sc = fresh_tmp(ce->ctx);
        ce_line(ce, "tur_adt_%s %s = (%s);", mn, sc, scrut);
        ce_line(ce, "tur_adt_%s *%s = &%s;", mn, sv, sc);
        free(sc);
    } else {
        ce_line(ce, "tur_adt_%s *%s = (tur_adt_%s *)(intptr_t)(%s);", mn, sv, mn, scrut);
    }
    free(scrut);

    uint32_t n = t->as.match.n_arms;
    bool chain_open = false;
    /* colored-call-inside-match-evicts-the-cps-backend: a single-constructor
     * record ADT carries no `tag` member (`struct tur_adt_Wrap { union { ...
     * } as; }`), so its one ctor arm is unconditional -- the same block form a
     * catch-all takes.  Only match_dk_ok's by-value admission reaches here
     * with n_ctors == 1. */
    bool tagged = adt->n_ctors >= 2;
    for (uint32_t i = 0; i < n; i++) {
        const CMatchArm *arm = &t->as.match.arms[i];
        if (arm->ctor && tagged) {
            if (!chain_open) { ce_line(ce, "if (%s->tag == %u) {", sv, arm->ctor->tag); chain_open = true; }
            else               ce_line(ce, "} else if (%s->tag == %u) {", sv, arm->ctor->tag);
        } else {
            /* catch-all: trailing `else`, or an unconditional block if it stands alone */
            if (chain_open) ce_line(ce, "} else {");
            else            ce_line(ce, "{");
        }
        ce->indent += 4;
        /* bind the pattern's field vars from the carrier struct */
        for (uint32_t bi = 0; bi < arm->n_fields; bi++) {
            const Binding *fb = arm->fields[bi];
            if (!fb) continue;
            const char *ctype = match_binder_c_type(&fb->type);
            char *bname = name_for_binding(ce->ctx, fb);
            char *mp = adt_field_member_path(arm->ctor->adt, arm->ctor, bi);
            /* SR1: an inline by-value aggregate field IS the aggregate in the
             * union slot -- bind it directly.  A cast-to-aggregate is invalid C,
             * so the general `(%s)` coercion below cannot be used for it. */
            if (byval && bi < arm->ctor->n_fields &&
                adt_field_is_inline_byval(&arm->ctor->fields[bi]))
                ce_line(ce, "%s %s = %s->%s;", ctype, bname, sv, mp);
            else
                ce_line(ce, "%s %s = (%s)%s->%s;", ctype, bname, ctype, sv, mp);
            free(mp); free(bname);
        }
        emit_term(ce, arm->body);
        ce->indent -= 4;
    }
    /* Close the chain.  With no trailing catch-all the match is exhaustive by
     * elaboration, so the fallthrough is unreachable (abort keeps -Wreturn quiet). */
    bool trailing_catchall = (n > 0 && (t->as.match.arms[n - 1].ctor == NULL || !tagged));
    if (chain_open && !trailing_catchall) {
        ce_line(ce, "} else {");
        ce->indent += 4;
        ce_line(ce, "abort(); /* exhaustive match: unreachable */");
        ce->indent -= 4;
    }
    ce_line(ce, "}");
    free(mn); free(sv);
}

/* Emit an owning-value op (rc/of, rc/drop, ...) by delegating to the direct
 * emitter.  The owning value is a local that never crosses a DK slot, so reusing
 * emit_value gives it the exact control-block / refcount / drop-glue discipline
 * the direct path uses -- no duplication, correct by construction. */
/* Name a CVar the way every reference site names it: through name_for_binding
 * when it stands for a source Binding, else its fresh cvar name.  Malloc'd. */
static char *cvar_cname(CE *ce, CVar x) {
    return x.bind ? name_for_binding(ce->ctx, x.bind) : strdup(x.name);
}

/* Name a CT_LETRAW binder consistently (see cvar_cname). */
static char *letraw_binder_name(CE *ce, const CTerm *t) {
    return cvar_cname(ce, t->as.letraw.x);
}

/* effectful-fnvalue-param-miscompile (E2): true when a CT_LETRAW value is an
 * EX_POLY_WRAP that emits a `tur_poly_fn_t` fat-closure struct literal (the
 * closure path or the wrapper-thunk path in emit_expr's EX_POLY_WRAP).  The
 * poly-wrap NODE is typed `ptr<void>` (the legacy thin carrier), so the CT-IR
 * binder derived from its type is `void *` -- but the emitted VALUE is the
 * multi-word `tur_poly_fn_t`, so `void *b = (tur_poly_fn_t){...}` is a hard C
 * error.  Declare the binder `tur_poly_fn_t` and pass it by value so the fat
 * closure (incl. its fn_cps channel) reaches the callee's `tur_poly_fn_t`
 * parameter intact. */
static bool letraw_emits_poly_fn(const CTerm *t) {
    if (!t || t->kind != CT_LETRAW) return false;
    const Expr *e = t->as.letraw.e;
    while (e && e->kind == EX_ASCRIBE) e = e->as.ascribe_.inner;
    if (!e || e->kind != EX_POLY_WRAP) return false;
    return e->as.poly_wrap_.is_closure || e->as.poly_wrap_.wrapper_binding != NULL;
}

static void emit_letraw(CE *ce, const CTerm *t) {
    /* B7: a `(set! m k)` that stores a continuation into a by-reference mutable is
     * emitted natively as a copy-on-store into the shared cell.  dk_perform frees
     * the captured chain when the handler case returns, so the stored continuation
     * is deep-copied (dk_copy_range) into an independent chain that outlives the
     * handler and can be resumed later.  Bypasses the direct-emitter delegation
     * (which would write the cell POINTER name without a deref). */
    /* A PLAIN `set!` into a by-reference mutable needs no special case here: the
     * direct emitter's store chokepoint (emit_set_stmt) derefs the cell, which
     * also covers a `set!` nested inside a delegated composite that never
     * reaches this lowering.  Only the CONTINUATION store is special, for its
     * value side. */
    const Binding *bref = byref_set_target(t->as.letraw.e);
    if (bref && is_byref_mut(bref)) {
        const Expr *se = t->as.letraw.e;
        while (se && se->kind == EX_ASCRIBE) se = se->as.ascribe_.inner;
        int si = ce->ctx->indent; ce->ctx->indent = ce->indent;
        char *val = emit_value(ce->ctx, ce->out, se->as.set_.value);
        ce->ctx->indent = si;
        char *cell = name_for_binding(ce->ctx, bref);
        /* __dk_reap_keep registers the cloned chain for a boundary dk_free (chain
         * walk) -- the stored continuation is resumed before the outermost entry
         * returns, then reaped, so it neither dangles nor leaks. */
        ce_line(ce, "*%s = (int64_t)(intptr_t)__dk_reap_keep(dk_copy_range((DK *)(intptr_t)(%s), NULL));",
                cell, val ? val : "0");
        char *bn2 = letraw_binder_name(ce, t);
        ce_line(ce, "%s = 0;", bn2);
        free(bn2); free(cell); free(val);
        emit_term(ce, t->as.letraw.body);
        return;
    }
    int saved = ce->ctx->indent;
    ce->ctx->indent = ce->indent;           /* line the delegated statements up */
    uint32_t dd_sum = ce->ctx->n_sum_pending, dd_any = ce->ctx->n_any_pending,
             dd_vsp = ce->ctx->n_vsp_pending;
    /* proper-tail-calls T6 (T-D6): `x = <dynamic call>; deliver x to KK_RET` is
     * a dynamic call in tail position.  In the main body of a bouncer it
     * bounces when `__kont` is the root the direct-entry wrapper armed, and
     * drives otherwise; anywhere else it drives.  Driving is always correct --
     * it is the ordinary call, made in a loop that absorbs the callee's own
     * bounces. */
    {
        const Expr *le = t->as.letraw.e;
        while (le && le->kind == EX_ASCRIBE) le = le->as.ascribe_.inner;
        const CTerm *nb = t->as.letraw.body;
        bool delivers_x = nb && nb->kind == CT_APPCONT &&
            nb->as.appcont.kont.kind == KK_RET &&
            ((nb->as.appcont.v.kind == CA_CVAR &&
              nb->as.appcont.v.cvar_id == t->as.letraw.x.id) ||
             (nb->as.appcont.v.kind == CA_VAR && t->as.letraw.x.bind &&
              nb->as.appcont.v.var == t->as.letraw.x.bind));
        if (le && le->kind == EX_DYN_CALL && le->as.dyn_call_.n_args <= TUR_FAT_SHIM_MAX_ARITY &&
            delivers_x && t->as.letraw.x.ty == TY_ANY) {
            bool bounce = ce->tb_bouncer && ce->out == ce->tb_out &&
                          !ce->ret_mode && !ce->shift_mode &&
                          ce->ctx->n_any_scope_drops == 0;
            ce->ctx->dyn_tail_mode = bounce ? DYN_TAIL_CPS : DYN_TAIL_DRIVE;
            ce->ctx->dyn_tail_guard = NULL;
        }
    }
    char *rhs = emit_value(ce->ctx, ce->out, t->as.letraw.e);
    ce->ctx->dyn_tail_mode = DYN_TAIL_NONE;
    ce->ctx->dyn_tail_guard = NULL;
    ce->ctx->indent = saved;
    /* cps-edge-walk-misses-nodes-and-colored-frames-leak: anything the
     * delegation queued for "drop after the consuming call" is ours now. */
    cps_deferred_capture(ce, t, dd_sum, dd_any, dd_vsp);
    char *bn = letraw_binder_name(ce, t);
    /* A nil-typed op yields a void/nil expression -- bind the unit placeholder
     * rather than assigning a void value.  But a nil/void EX_CALL is a special
     * case: emit_value returns the call as an UN-emitted expression (a void
     * result is not hoisted into a temp -- see emit_value), so its side effect is
     * only realized if we emit it as a statement here.  Other nil ops (rc/drop,
     * set!, while, ...) already emitted their statements and return a unit
     * placeholder, so they must NOT be re-emitted (that would double the effect). */
    if (t->as.letraw.x.ty == TY_NIL || t->as.letraw.x.ty == TY_NEVER) {
        /* TY_NEVER: a diverging op (a `panic`), whose emit_value already emitted
         * the abort statement and returned a void placeholder -- bind the nil word
         * (0), never `x = ((void)0)`.  Same shape as the nil case. */
        const Expr *le = t->as.letraw.e;
        while (le && le->kind == EX_ASCRIBE) le = le->as.ascribe_.inner;
        /* B8: a nil-returning inline-C op (e.g. a session `close`, whose value is
         * `:nil`) is returned by emit_value as an un-emitted expression exactly
         * like a void EX_CALL -- so its side effect must be realized here, or the
         * op is silently dropped (a delegated `(close ch)` never runs, leaking the
         * channel).  Emit it as a statement, same as the EX_CALL case. */
        if (le && (le->kind == EX_CALL || le->kind == EX_INLINE_C) && rhs && rhs[0])
            ce_line(ce, "%s;", rhs);
        ce_line(ce, "%s = 0;", bn);
    } else if (t->as.letraw.x.ty == TY_FN) {
        /* A boxed fn value (a fat closure / fn-pointer word, e.g. a cross-function
         * __Shift receiver built by the direct emitter) rides the one-word DK slot
         * as an int64_t binder, but emit_value yields it as a pointer expression --
         * cast through intptr_t so the store is a clean integer, not a
         * -Wint-conversion pointer->int assignment. */
        ce_line(ce, "%s = (int64_t)(intptr_t)(%s);", bn, rhs ? rhs : "0");
    } else {
        /* cps-result-unbox-dropped: mirror the direct emitter's
         * `init_carrier_to_byval` bridge (emit_expr.c, EX_LET).
         *
         * An inline-C defn whose result is a by-value ADT app -- `(Result T E)`
         * / `(Option T)` under lowering -- is EMITTED as the int64 carrier (a
         * malloc'd `tur_result_box_t *`), while the binder that receives it is
         * declared as the by-value aggregate.  The direct emitter derefs the
         * carrier into the aggregate at the initializer; this delegated path
         * assigned it raw, so `cc` rejected
         *
         *   error: assigning to 'tur_adt_Result__T__E' from incompatible type
         *          'int64_t'
         *
         * and every caller of a higher-order function (which is what forces the
         * CPS transform) hit it.  Same gate as the direct site, so it is inert
         * whenever the init already yields the aggregate.
         *
         * cps-let-binder-bridge-lacks-position-check: "same gate as the direct
         * site" stopped being true when that site gained a POSITION check --
         * `fn_body_tail_emits_byvalue_carrier_abi` asks what the Expr would
         * naturally emit, not what the value in hand already is, and a value
         * something else already bridged needs the second question.  The term is
         * restored below, so the claim above holds again.
         *
         * Measured before adding it, since a change to a path with no failing
         * case is otherwise unverifiable: across all 2131 fixtures only 33 reach
         * this bridge with a by-value init type at all, and in every one of them
         * either the tail predicate already suppresses it or the init is recorded
         * as `int64_t` / a pointer / nothing -- never as the aggregate.  So this
         * term changes no emitted byte in the corpus today; it is a consistency
         * repair that keeps the two sites from drifting again, not a fix for an
         * observed miscompile. */
        const char *bct = binder_ctype_full(ce->ctx, t->as.letraw.x.ty,
                                            t->as.letraw.x.type);
        Type init_bv = fn_body_tail_byvalue_carrier_type(ce->ctx, t->as.letraw.e);
        bool bridged_ok = false;
        if (rhs && bct && strcmp(bct, "int64_t") != 0 &&
            strchr(bct, '*') == NULL &&
            init_bv.kind != TY_UNKNOWN &&
            !emit_value_is_recorded_as(rhs, bct) &&
            !fn_body_tail_emits_byvalue_carrier_abi(ce->ctx, t->as.letraw.e)) {
            int saved = ce->ctx->indent;
            ce->ctx->indent = ce->indent;
            char *br = emit_carrier_bridge(ce->ctx, ce->out, strdup(rhs),
                                           CK_CARRIER, CK_CONCRETE, init_bv);
            ce->ctx->indent = saved;
            if (br) {
                ce_line(ce, "%s = %s;", bn, br);
                free(br);
                bridged_ok = true;
            }
        }
        /* generic-call-result-in-generic-collapses-to-int, the CPS half: the
         * binder is the int64 carrier word (typed from a wrapped tyvar-result
         * call's `int`) but the delegated value is a temp the direct emitter
         * RECORDED as a concrete `double` / `float` / by-value aggregate -- a
         * concrete spec's result.  Assigning it raw value-converted the double
         * (3.45846e-323) or was invalid C for the aggregate.  Pack it the way
         * the DK slot carries such a value: float bits, or a reaped heap box
         * (the same box `dk_run` hands an aggregate result through). */
        if (!bridged_ok && rhs && bct && strcmp(bct, "int64_t") == 0 &&
            emit_str_is_bare_ident(rhs)) {
            const char *rc = emit_localvar_lookup_ctype(rhs);
            if (rc && (strcmp(rc, "double") == 0 || strcmp(rc, "float") == 0)) {
                char *br = emit_carrier_bridge(ce->ctx, ce->out, strdup(rhs),
                    CK_CONCRETE, CK_CARRIER,
                    emit_type_from_kind(strcmp(rc, "float") == 0 ? TY_FLOAT32
                                                                : TY_FLOAT));
                ce_line(ce, "%s = %s;", bn, br);
                free(br);
                bridged_ok = true;
            } else if (rc && strncmp(rc, "tur_adt_", 8) == 0 && !strchr(rc, '*')) {
                ce_line(ce, "%s = __dk_reap_ptr((intptr_t)({ %s *__bx = (%s *)malloc(sizeof(%s)); *__bx = (%s); __bx; }));",
                        bn, rc, rc, rc, rhs);
                bridged_ok = true;
            } else if (rc && strlen(rc) >= 1 && rc[strlen(rc) - 1] == '*') {
                /* A pointer into the carrier word: the word IS the pointer.
                 * Two routes reach it -- a cstr spec's `const char *` result
                 * (thunk/ident/cstr under clang), and a binder the lowering
                 * typed from an erasing `(:: x :int)` it then PEELED off the
                 * value (cps_bind), so a typed pointer -- `tur_adt_Cons__int *`
                 * from a spec'd call -- lands in an `int64_t`; the implicit
                 * erasure a `:heap` node takes into any `:int` parameter
                 * (elab_call.c, security-audit WP5) made that common. */
                ce_line(ce, "%s = (int64_t)(intptr_t)(%s);", bn, rhs);
                bridged_ok = true;
            }
        }
        /* cps-letraw-pointer-binder-from-carrier: the binder is a concrete
         * POINTER (a spec clone's `tur_adt_Vec__float *` local) and the value is
         * a temp recorded as the int64 carrier word (`vec_hynew()`'s result).
         * The word IS the pointer; assigning it raw was an int-conversion error
         * under clang and gcc 14 (gcc 13 only warns). */
        if (!bridged_ok && rhs && bct && emit_str_is_bare_ident(rhs)) {
            size_t bL = strlen(bct);
            const char *rc = emit_localvar_lookup_ctype(rhs);
            if (bL >= 1 && bct[bL - 1] == '*' && rc && strcmp(rc, "int64_t") == 0) {
                ce_line(ce, "%s = (%s)(intptr_t)(%s);", bn, bct, rhs);
                bridged_ok = true;
            }
        }
        if (!bridged_ok)
            ce_line(ce, "%s = %s;", bn, rhs ? rhs : "0");
    }
    /* reap_env (cps_closure_env_freeable): a leaf-admitted, provably non-escaping
     * capturing closure whose heap fat-env the direct emitter did NOT free at
     * this leaf position (only emit_value(EX_LET) applies the scoped free).  The
     * bound value is the malloc'd env pointer; register it for a single-node free
     * at the outermost DK entry boundary (dead after its lifted body; boundary
     * reap never double-frees, and never walks -- __dk_reap_ptr is a bare free).
     * A scalar-captured closure frees cleanly; the freeable gate already excluded
     * a non-scalar-returning closure (result could alias the env). */
    bool loop_reap_pushed = false;
    if (t->as.letraw.reap_env) {
        /* closure-drop-glue: flag-on this env is headered (env[-1] drop-glue),
         * so reap it as a headered closure (kind 2 -> TUR_CLOSURE_DROP: recovers
         * the header, walks owning captures, frees the base) rather than a bare
         * interior free. */
        ce_line(ce, "__dk_reap_closure((intptr_t)%s);", bn);
        if (t->as.letraw.reap_at_backedge && ce->self_out && ce->out == ce->self_out &&
            ce->n_loop_reaps < sizeof ce->loop_reaps / sizeof ce->loop_reaps[0]) {
            ce->loop_reaps[ce->n_loop_reaps++] = strdup(bn);
            loop_reap_pushed = true;
        }
    }
    /* dynamic-returned-closure-env-is-never-freed: an `any` binder holding a
     * fresh capturing closure; its payload word is the headered env.
     *
     * Registered only when `__kont` is not the trampoline root.  A dynamic
     * tail call bounces exactly when it is (the T6 check in emit_value's
     * DYN_TAIL_CPS arm), and a bounce hands the callee -- possibly this
     * closure, or a callee holding it -- to the driver, which runs it after
     * this entry, and the reap at its exit, are done.  Every tail call from
     * here threads `__kont` (or a prompt chain made inside this function), so
     * when it is not the root nothing reached from here can bounce out, and
     * the closure is dead by the time the entry returns.  Only the MAIN body
     * has `__kont` as its own continuation parameter; a lifted helper leaves
     * the env to leak, which is the status quo. */
    if (t->as.letraw.reap_any_env && ce->out == ce->tb_out &&
        !ce->shift_mode && !ce->ret_mode && !ce->handler_case_mode) {
        ensure_saffron_dyn_runtime(ce->ctx);   /* declares tur_tb_root */
        ce_line(ce, "if ((void *)__kont != tur_tb_root) "
                    "__dk_reap_closure((intptr_t)TUR_UNTAG(%s));", bn);
    }
    emit_letraw_fnfield_reap(ce, t, bn);
    free(bn);
    free(rhs);
    emit_term(ce, t->as.letraw.body);
    if (loop_reap_pushed) free(ce->loop_reaps[--ce->n_loop_reaps]);
}

/* struct-temporary-fn-field-box-leaks (the CPS half): a by-value struct
 * local the elaborator flagged `drops_fn_fields` -- non-escaping, so its boxed
 * fn fields die with it (local-struct-drop) -- is released by the direct
 * emitter's `drop_fnfields_<T>` at scope end.  Here the scope can end in a
 * tail call (often the very call through the field), and nothing runs after
 * one, so each boxed field is registered with the entry boundary's reap
 * instead, as a headered closure (kind 2) -- the same channel `reap_env` uses
 * for a non-escaping closure env.  Reaping later than the scope's end is
 * safe for a value nothing outside the scope can name. */
static void emit_letraw_fnfield_reap(CE *ce, const CTerm *t, const char *bn) {
    const Binding *xb = t->as.letraw.x.bind;
    if (!xb || !xb->drops_fn_fields || xb->type.kind != TY_ADT) return;
    const AdtDef *def = xb->type.as.adt_.def;
    if (!def || def->n_ctors < 1 || !def->ctors[0]) return;
    if (t->as.letraw.x.ty == TY_NIL || t->as.letraw.x.ty == TY_NEVER ||
        t->as.letraw.x.ty == TY_FN)
        return;
    /* Only a binder declared as the aggregate itself: a carrier word or a
     * pointer names storage this binder does not own. */
    const char *bct = binder_ctype_full(ce->ctx, t->as.letraw.x.ty,
                                        t->as.letraw.x.type);
    if (!bct || strncmp(bct, "tur_adt_", 8) != 0 || strchr(bct, '*')) return;
    const CtorDef *ctor = def->ctors[0];
    for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
        const Type *ft = ctor->fields[fi].full_type;
        if (ctor->fields[fi].kind != TY_FN || !ft || ft->kind != TY_FN ||
            !ft->as.fn.boxed)
            continue;
        char *mp = adt_field_member_path(def, ctor, fi);
        ce_line(ce, "__dk_reap_closure((intptr_t)(%s).%s);", bn, mp);
        free(mp);
    }
}

/* ---- binder pre-declaration ------------------------------------------ *
 * Every let-bound C variable is declared (uninitialized) at the top of the
 * function so that forward `goto`s into a join never jump past a declaration.
 * All values thread as machine scalars, so int64_t is the uniform storage. */
static void emit_binder_decls(CE *ce, const CTerm *t) {
    if (!t) return;
    switch (t->kind) {
        case CT_APPCONT: break;
        case CT_LETVAL: {
            char *bn = cvar_cname(ce, t->as.letval.x);
            /* B7: a by-reference mutable's binder is the (typed) cell POINTER. */
            if (is_byref_mut(t->as.letval.x.bind))
                ce_line(ce, "%s%s;", byref_cell_ptr_ctype(ce->ctx, t->as.letval.x.bind), bn);
            else
                { const char *__bct = binder_ctype_full(ce->ctx, t->as.letval.x.ty, t->as.letval.x.type); ce_line(ce, "%s %s;", __bct, bn); /* cps-binder-ctype-recorded: the direct emitter's carrier rules (a typed pointer into an int64 slot) read this. */ emit_localvar_record_ctype(bn, __bct); }
            free(bn);
            emit_binder_decls(ce, t->as.letval.body);
            break;
        }
        case CT_LETPRIM: {
            char *bn = cvar_cname(ce, t->as.letprim.x);
            { const char *__bct = binder_ctype_full(ce->ctx, t->as.letprim.x.ty, t->as.letprim.x.type); ce_line(ce, "%s %s;", __bct, bn); /* cps-binder-ctype-recorded: the direct emitter's carrier rules (a typed pointer into an int64 slot) read this. */ emit_localvar_record_ctype(bn, __bct); }
            free(bn);
            emit_binder_decls(ce, t->as.letprim.body);
            break;
        }
        case CT_LETCALL: {
            char *bn = cvar_cname(ce, t->as.letcall.x);
            { const char *__bct = binder_ctype_full(ce->ctx, t->as.letcall.x.ty, t->as.letcall.x.type); ce_line(ce, "%s %s;", __bct, bn); /* cps-binder-ctype-recorded: the direct emitter's carrier rules (a typed pointer into an int64 slot) read this. */ emit_localvar_record_ctype(bn, __bct); }
            free(bn);
            emit_binder_decls(ce, t->as.letcall.body);
            break;
        }
        case CT_LETRAW: {
            char *bn = t->as.letraw.x.bind
                ? name_for_binding(ce->ctx, t->as.letraw.x.bind)
                : strdup(t->as.letraw.x.name);
            /* E2: a poly-wrap value is the fat `tur_poly_fn_t`, not the void*
             * carrier its ptr<void> node type would spell. */
            if (letraw_emits_poly_fn(t)) {
                ce_line(ce, "tur_poly_fn_t %s;", bn);
                if (!t->as.letraw.x.bind && g_n_polyfn_cvars < 256)
                    g_polyfn_cvar_ids[g_n_polyfn_cvars++] = t->as.letraw.x.id;
            }
            else
                { const char *__bct = binder_ctype_full(ce->ctx, t->as.letraw.x.ty, t->as.letraw.x.type); ce_line(ce, "%s %s;", __bct, bn); /* cps-binder-ctype-recorded: the direct emitter's carrier rules (a typed pointer into an int64 slot) read this. */ emit_localvar_record_ctype(bn, __bct); }
            free(bn);
            emit_binder_decls(ce, t->as.letraw.body);
            break;
        }
        case CT_TAILCALL: break;
        case CT_LETCONT: {
            if (letcont_is_heap_join(t)) break;  /* param + jbody are lifted into a frame helper */
            if (letcont_is_escaping_join(t)) {
                /* param + jbody live in the resume-frame helper; the body stays. */
                emit_binder_decls(ce, t->as.letcont.body);
                break;
            }
            /* Name the slot via cvar_cname so a source-Binding param mangles
             * consistently with the delivery + the join body's references (see
             * the CT_LETCONT emit in emit_term); the raw param.name would be an
             * invalid C identifier for a kebab-case `let` binder. */
            char *pn = cvar_cname(ce, t->as.letcont.param);
            { const char *__bct = binder_ctype_full(ce->ctx, t->as.letcont.param.ty, t->as.letcont.param.type); ce_line(ce, "%s %s;", __bct, pn); /* cps-binder-ctype-recorded: the direct emitter's carrier rules (a typed pointer into an int64 slot) read this. */ emit_localvar_record_ctype(pn, __bct); }
            free(pn);
            emit_binder_decls(ce, t->as.letcont.body);
            emit_binder_decls(ce, t->as.letcont.jbody);
            break;
        }
        case CT_IF:
            emit_binder_decls(ce, t->as.if_.then_);
            emit_binder_decls(ce, t->as.if_.else_);
            break;
        case CT_MATCH:
            /* The scrutinee temp + pattern field bindings are declared INLINE at
             * the emit site (block-scoped inside each arm); only the arm bodies'
             * CPS binders (CT_LETVAL/LETPRIM/... assigned, not declared, in
             * emit_term) need the function-scope forward declaration. */
            for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                emit_binder_decls(ce, t->as.match.arms[i].body);
            break;
        case CT_RESET:
            /* Only the delimited body lives in this function; reset.body is
             * lifted into a helper with its own decls. */
            emit_binder_decls(ce, t->as.reset.delim);
            break;
        case CT_SHIFT: break;   /* shift.body is lifted into a helper */
        case CT_HANDLE:
            /* Only the handled body lives here; the continuation and case are
             * lifted into helpers. */
            emit_binder_decls(ce, t->as.handle.delim);
            break;
        case CT_PERFORM: break;   /* terminal; the continuation is lifted */
        case CT_AWAIT:   break;   /* F3: terminal; the continuation is lifted */
        case CT_RESUME: {
            char *bn = cvar_cname(ce, t->as.resume.x);
            { const char *__bct = binder_ctype_full(ce->ctx, t->as.resume.x.ty, t->as.resume.x.type); ce_line(ce, "%s %s;", __bct, bn); /* cps-binder-ctype-recorded: the direct emitter's carrier rules (a typed pointer into an int64 slot) read this. */ emit_localvar_record_ctype(bn, __bct); }
            free(bn);
            emit_binder_decls(ce, t->as.resume.body);
            break;
        }
        case CT_CLONEABLE: {
            char *bn = cvar_cname(ce, t->as.cloneable.x);
            { const char *__bct = binder_ctype_full(ce->ctx, t->as.cloneable.x.ty, t->as.cloneable.x.type); ce_line(ce, "%s %s;", __bct, bn); /* cps-binder-ctype-recorded: the direct emitter's carrier rules (a typed pointer into an int64 slot) read this. */ emit_localvar_record_ctype(bn, __bct); }
            free(bn);
            emit_binder_decls(ce, t->as.cloneable.body);
            break;
        }
        case CT_CALLCC: {
            char *bn = cvar_cname(ce, t->as.callcc.x);
            { const char *__bct = binder_ctype_full(ce->ctx, t->as.callcc.x.ty, t->as.callcc.x.type); ce_line(ce, "%s %s;", __bct, bn); /* cps-binder-ctype-recorded: the direct emitter's carrier rules (a typed pointer into an int64 slot) read this. */ emit_localvar_record_ctype(bn, __bct); }
            free(bn);
            emit_binder_decls(ce, t->as.callcc.body);
            break;
        }
        case CT_LOOP:     break;   /* cps-while-native: loop body decls live in the helper fn */
        case CT_CONTINUE: break;   /* terminal back-edge -- no binders */
        default: break;
    }
}

/* ---- C3: lifted reset/shift helpers ---------------------------------- *
 * A reset's continuation and a shift's body are reified as file-scope helper
 * functions (matching the DKFrame / DKBody signatures) so the DK machine can
 * invoke them.  The zero-capture cut passes the enclosing continuation `k`
 * directly as the frame env; a body needing to capture other locals is rejected
 * at classification time (has_capture) and falls back. */

/* The shape of a lifted helper. */
typedef enum {
    LH_SHIFT_BODY,    /* DKBody  (env, subk):   shift body, KK_PROMPT -> return value */
    LH_PERFORM_CONT,  /* DKFrame (env, xval):   perform continuation, KK_RET -> return value */
    LH_HANDLER_CASE,  /* DKHandler (env, arg, subk): binds params+k, KK_PROMPT -> return */
    LH_RESUME_CONT,   /* DKResumeFrame (env, xval, __kont): a frame that receives its
                       * run-time downstream chain `__kont` and threads it (KK_RET ->
                       * dk_run(__kont,..); a nested perform/shift threads __kont).  Caps
                       * ride env (no __k).  Users: a MULTI-SUSPENSION perform
                       * continuation (Track A: its body contains a nested control op),
                       * and every HANDLE, RESET, and bounded-AWAIT continuation (spine
                       * unification: the frame's `next` is the real, borrowed enclosing
                       * chain, so a dk_copy_range copy threads its OWN tail instead of a
                       * baked original -- the multishot-across-nested-handle fix and its
                       * reset/await riders).  The old LH_RESET_CONT (env-baked `__k` +
                       * marker `next`) is deleted: nothing may bake an original-chain
                       * pointer into a frame env again. */
} LHMode;

/* Effect re-opening: does this handler CASE body itself perform an effect (which,
 * being unhandled by its own handle, reaches an ENCLOSING handler)?  A re-opening
 * case needs `__kont` -- the enclosing handler markers -- declared at entry so
 * emit_perform's cur_k resolves.  Scans the case's own straight-line/branch
 * structure (handle_case_ok admits no nested handle in a case body, so there is
 * no inner effect scope to descend past). */
static bool case_reopens(const CTerm *t) {
    while (t) {
        switch (t->kind) {
            case CT_PERFORM: return true;
            case CT_LETVAL:  t = t->as.letval.body;  break;
            case CT_LETPRIM: t = t->as.letprim.body; break;
            case CT_LETCALL: t = t->as.letcall.body; break;
            case CT_RESUME:  t = t->as.resume.body;  break;
            case CT_LETRAW:  t = t->as.letraw.body;  break;
            case CT_CALLCC:  t = t->as.callcc.body;  break;
            case CT_IF:
                return case_reopens(t->as.if_.then_) || case_reopens(t->as.if_.else_);
            default: return false;
        }
    }
    return false;
}

/* Emit one lifted helper into ce->helpers.  `xname` is the incoming value
 * parameter name (reset/perform continuation result var); `xt` its full Type
 * (may be NULL for a scalar) so a Tier C aggregate value-param unboxes.  `hcase`
 * is the handler clause (for LH_HANDLER_CASE param/k binding), else NULL.
 * `caps` (N6.3) is the set of scalar source captures this body needs; when
 * non-empty (LH_PERFORM_CONT only, for now) the `env` slot carries a heap struct
 * of those values, which the helper reads back into real-typed locals. */
static void emit_lifted(CE *ce, const char *name, LHMode mode,
                        const char *xname, TypeKind xty, const Type *xt,
                        const CTerm *body, const CHandleCase *hcase,
                        const CapSet *caps) {
    bool has_caps = (caps && caps->n > 0);
    /* fn-value-fat-normalization (effect-row increment): a lifted helper is its
     * OWN function -- captures arrive through its frame env (`__cap->fN`) as
     * raw-named locals, never through the enclosing closure's env pointer.
     * When this helper is lifted out of a capturing lambda's __cps body, the
     * ctx closure context (which routes capture reads through `__env_X->cap`)
     * must not leak in: it would spell the loader's LOCAL name as a deref
     * (`int64_t __env_X->w = __cap->f0;` -- invalid C).  The frame FILL back in
     * the enclosing body still runs under the closure context and reads
     * `__env_X->w` correctly. */
    struct Closure *saved_lh_closure = ce->ctx->closure;
    const char *saved_lh_env_var = ce->ctx->env_var_name;
    ce->ctx->closure = NULL;
    ce->ctx->env_var_name = NULL;
    /* A RE-OPENING case (its body performs an effect its own handle does not
     * handle) runs with the REAL enclosing chain as `__kont`
     * (dk_case_enclosing_real) and delivers its value through it on every exit
     * -- the case_delivers protocol (emit_handle wraps its handler node in
     * dk_case_delivers; dk_perform then returns the case's result without a
     * second H->next delivery).  So its KK_PROMPT exits must emit
     * dk_run(__kont, v), i.e. shift_mode OFF, unlike a plain case whose value
     * returns to dk_perform for delivery.  See
     * docs/archive/cps-case-reopen-marker-kont-truncates-capture.md. */
    bool reopens = (mode == LH_HANDLER_CASE) && case_reopens(body);
    /* Emit the body into a temporary buffer first, so any nested reset/shift/
     * effect appends its own (inner) helpers ahead of this one in ce->helpers. */
    Buf tmp; buf_init(&tmp);
    CE hc = *ce;
    hc.out = &tmp;
    hc.indent = 4;
    hc.n_joins = 0;
    hc.cur_k = "__kont";
    /* A perform continuation returns its value however it is delivered (KK_RET
     * or KK_PROMPT), so it sets both return modes. */
    hc.shift_mode = (mode == LH_SHIFT_BODY || mode == LH_PERFORM_CONT
                     || (mode == LH_HANDLER_CASE && !reopens));
    hc.ret_mode   = (mode == LH_PERFORM_CONT);
    hc.handler_case_mode = (mode == LH_HANDLER_CASE);   /* E7: only a direct case body */
    hc.case_tail_resume  = (mode == LH_HANDLER_CASE) ? ce->case_tail_resume : false;

    /* N6.3: read the captured values out of the env struct into locals named the
     * same way the body references them (name_for_binding). */
    if (has_caps) {
        indent_buf(&tmp, 4);
        buf_printf(&tmp, "%s_env *__cap = (%s_env *)(intptr_t)env;\n", name, name);
        for (int i = 0; i < caps->n; i++) {
            char *cn = caps->b[i] ? name_for_binding(ce->ctx, caps->b[i]) : strdup(caps->cvname[i]);
            indent_buf(&tmp, 4);
            buf_printf(&tmp, "%s %s = __cap->f%d;\n", cap_ctype(ce->ctx, caps, i), cn, i);
            /* The direct emitter, which renders this helper's delegated
             * expressions, keys its bridges on a local's RECORDED spelling;
             * without it a capture declared `tur_adt_Vec__float *` here was
             * passed bare into an inline-C callee's int64 parameter. */
            emit_localvar_record_ctype(cn, cap_ctype(ce->ctx, caps, i));
            /* E1 (Option A): an owning capture admitted into a multi-shot
             * continuation is CLONED (increfed) on read-out, so each invocation of
             * this helper owns its own +1 that the body's drop balances.  The env's
             * original +1 is intentionally leaked (matching the leaked-DK-node
             * regime); the fixture carries requires.no-leak-check.  Currently only
             * an rc handle reaches here (cap_owning_ok gates it). */
            if (caps->owning[i]) {
                indent_buf(&tmp, 4);
                buf_printf(&tmp, "rc_strong_increment(%s);\n", cn);
            }
            free(cn);
        }
    }

    /* Load the incoming value out of the one-word slot into a real-typed local
     * (reset/perform continuation), or bind the handler case's params/k.  The
     * value-param load leaks a Tier C box (consume=false): this frame can run
     * more than once under a multi-shot resume. */
    if (mode == LH_PERFORM_CONT || mode == LH_RESUME_CONT) {
        char slotexpr[160];
        snprintf(slotexpr, sizeof slotexpr, "%s__slot", xname);
        char *ld = slot_load(ce->ctx, xty, xt, slotexpr, false);
        indent_buf(&tmp, 4);
        buf_printf(&tmp, "%s %s = %s;\n", binder_ctype_full(ce->ctx, xty, xt), xname, ld);
        free(ld);
    }
    if (mode == LH_HANDLER_CASE && hcase) {
        if (hcase->n_params == 1) {
            const Binding *pb = hcase->params[0];
            char *pn = name_for_binding(ce->ctx, pb);
            const char *pty = binder_ctype_full(ce->ctx, pb->type.kind, &pb->type);
            char *ld = slot_load(ce->ctx, pb->type.kind, &pb->type, "arg", false);
            indent_buf(&tmp, 4);
            buf_printf(&tmp, "%s %s = %s;\n", pty, pn, ld);
            free(ld);
            free(pn);
        } else if (hcase->n_params > 1) {
            /* Multi-arg effect: `arg` is a pointer to the heap-packed
             * `int64_t[n]` slot-word array built at the perform site; unpack each
             * param from its slot (value-load, consume=false -- a multi-shot
             * resume may re-read the array). */
            indent_buf(&tmp, 4);
            buf_puts(&tmp, "int64_t *__eargs = (int64_t *)arg;\n");
            for (uint32_t pi = 0; pi < hcase->n_params; pi++) {
                const Binding *pb = hcase->params[pi];
                char *pn = name_for_binding(ce->ctx, pb);
                const char *pty = binder_ctype_full(ce->ctx, pb->type.kind, &pb->type);
                char src[32];
                snprintf(src, sizeof src, "__eargs[%u]", pi);
                char *ld = slot_load(ce->ctx, pb->type.kind, &pb->type, src, false);
                indent_buf(&tmp, 4);
                buf_printf(&tmp, "%s %s = %s;\n", pty, pn, ld);
                free(ld);
                free(pn);
            }
        }
        if (hcase->k) {
            char *kn = name_for_binding(ce->ctx, hcase->k);
            indent_buf(&tmp, 4);
            /* cps-dk-multishot-user-effects (Phase A): a __Shift receiver OR a
             * resumable-payload user handler binds `k` as a DK-backed cloneable
             * cont so the payload's `(k v)` / `resume` resumes the DK chain through
             * tur_cloneable_cont_resume.  Both take the same wrap + boundary reaps
             * (the `arg` reap is safe because `arg` is a boxed fn payload here). */
            if (is_shift_effect(hcase->effect) || hcase->resumable_payload) {
                /* Cross-function `shift` bridge crux: the __Shift receiver
                 * (`recv`) is a direct-emitted `(fn [k : cont] ...)` that resumes
                 * its continuation via `tur_cloneable_cont_resume(
                 * tur_continuation_snapshot(k), v)`.  Its `k` must therefore be a
                 * `tur_cloneable_cont`, not the raw DK `subk`.  Wrap subk with the
                 * DK<->cloneable bridge (__dk_cont_fn / __dk_env_clone /
                 * __dk_env_drop, emit_cps_cloneable_bridge_prelude): the bound `k`
                 * is a cloneable cont whose env is an OWNED copy of the DK chain;
                 * each `(k v)` snapshots (clones the chain via __dk_env_clone =
                 * dk_copy_range) before resuming, so a multi-shot receiver
                 * (`(+ (k 1) (k 2))`) resumes an independent chain per call.
                 *
                 * The bound `k` (the cont struct + its owned DK-chain env) is only
                 * ever cloned per resume -- never itself resumed -- so it is dead
                 * once the receiver returns.  Hoist the env so both halves can be
                 * reclaimed at the outermost entry boundary (by then every resume
                 * has settled): the chain env via __dk_reap_keep (dk_free -- it is
                 * a self-contained dk_copy_range copy), the struct via __dk_reap_ptr
                 * (plain free -- tur_cloneable_cont_alloc malloc'd it).  Together
                 * these are tur_cloneable_cont_drop deferred to the boundary, so
                 * the per-receiver cont no longer leaks
                 * (docs/archive/cps-runtime-finish-plan.md, P3.c). */
                buf_printf(&tmp, "DK *%s_kenv = dk_copy_range(subk, NULL);\n", kn);
                indent_buf(&tmp, 4);
                buf_printf(&tmp, "int64_t %s = (int64_t)(intptr_t)tur_cloneable_cont_alloc("
                                 "__dk_cont_fn, %s_kenv, __dk_env_clone, __dk_env_drop);\n", kn, kn);
                indent_buf(&tmp, 4);
                buf_printf(&tmp, "__dk_reap_keep(%s_kenv);\n", kn);
                indent_buf(&tmp, 4);
                buf_printf(&tmp, "__dk_reap_ptr((intptr_t)%s);\n", kn);
                /* P3.d (escaping-fat-closure-env free, scoped to __Shift): the
                 * receiver -- the effect argument `arg` -- is the shift's
                 * `(fn [k] ...)`, which the __Shift desugar ALWAYS boxes into a
                 * fresh heap value: a capturing-closure env, or an EX_FN_TO_FAT
                 * fatshim (`malloc(2*int64)`) for a bare / capture-free fn (never a
                 * raw code pointer).  It is consumed exactly once here (`recv(k)`)
                 * and is not reclaimed at the perform site, so reap its env at the
                 * outermost entry boundary (plain free -- it is a bare malloc).
                 * A scalar-captured receiver frees cleanly; an owning capture
                 * leaks its captured value (no closure drop glue yet) but never
                 * double-frees -- the conservative interim of
                 * docs/archive/escaping-fat-closure-env-leak.md, applied to the
                 * one non-escaping closure the CPS backend itself constructs.
                 *
                 * closure-drop-glue: flag-on this receiver box is headered
                 * (env[-1] drop-glue; fat pointer PAST it), so a bare free is an
                 * interior free.  Reap it as a headered closure (kind 2 ->
                 * TUR_CLOSURE_DROP: recovers the header, walks owning captures --
                 * also closing the "owning capture leaks" caveat above -- frees
                 * the base). */
                indent_buf(&tmp, 4);
                buf_puts(&tmp, "__dk_reap_closure((intptr_t)arg);\n");
            } else {
                /* Bind `k` as the int64_t word its Binding is typed as -- not
                 * `DK *`.  Every read site casts (`dk_invoke((DK *)(k), ...)`,
                 * `((DK *)(k))->consumed`, the `cont?` check), so nothing needs
                 * the pointer spelling, and every place the local can FLOW
                 * needs the word: a perform-continuation env field (the
                 * re-opening case, which always bound it this way), and a
                 * loop-invariant helper param when a `while` in the case body
                 * resumes k per iteration (`int64_t k_<id>` in the synthesized
                 * loop helper -- a `DK *` local there is an int-from-pointer
                 * hard error under GCC >= 14 at the entry call). */
                buf_printf(&tmp, "int64_t %s = (int64_t)(intptr_t)subk;\n", kn);
            }
            free(kn);
        }
    }
    /* Effect re-opening: a case body that performs an outer-handled effect needs
     * its REAL enclosing chain as `__kont`.  dk_perform set
     * g_dk_case_reopen_hnode to this case's handler node just before calling us;
     * read it (into a local, at entry, before any interior perform can overwrite
     * the global).  The chain is BORROWED (dk_case_enclosing_real -- no copy, no
     * reap): the interior effect dispatches into the real enclosing handlers, its
     * capture crosses into the real intermediate frames, and this case's exits
     * deliver through the same chain (shift_mode off above), so the rest of the
     * program is chain-reachable -- not a marker dead-end plus a C-stack frame
     * that a tail-resume longjmp could discard. */
    if (reopens) {
        indent_buf(&tmp, 4);
        buf_puts(&tmp, "DK *__kont = dk_case_enclosing_real(g_dk_case_reopen_hnode);\n");
    }
    emit_binder_decls(&hc, body);
    emit_term(&hc, body);
    buf_putc(&tmp, '\0');

    /* N6.3: the env struct type (named <name>_env) shared with the alloc site.
     * Caps-only -- no continuation slot (see emit_cont_env). */
    if (has_caps) {
        buf_printf(ce->helpers, "typedef struct {");
        for (int i = 0; i < caps->n; i++)
            buf_printf(ce->helpers, " %s f%d;", cap_ctype(ce->ctx, caps, i), i);
        buf_printf(ce->helpers, " } %s_env;\n", name);
    }

    switch (mode) {
        case LH_SHIFT_BODY:
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, DK *subk) {\n", name);
            if (has_caps) buf_puts(ce->helpers, "    (void)subk;\n");
            else          buf_puts(ce->helpers, "    (void)env; (void)subk;\n");
            break;
        case LH_HANDLER_CASE:
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, intptr_t arg, DK *subk) {\n", name);
            if (has_caps) buf_puts(ce->helpers, "    (void)arg; (void)subk;\n");
            else          buf_puts(ce->helpers, "    (void)env; (void)arg; (void)subk;\n");
            break;
        case LH_PERFORM_CONT:
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, intptr_t %s__slot) {\n", name, xname);
            if (!has_caps) buf_puts(ce->helpers, "    (void)env;\n");
            break;
        case LH_RESUME_CONT:
            /* Track A: a resume-frame.  __kont is the run-time downstream chain
             * (a DKResumeFrame parameter, not the env); caps (if any) ride env. */
            buf_printf(ce->helpers, "static intptr_t %s(intptr_t env, intptr_t %s__slot, DK *__kont) {\n", name, xname);
            if (!has_caps) buf_puts(ce->helpers, "    (void)env;\n");
            break;
    }
    buf_puts(ce->helpers, tmp.data);
    buf_puts(ce->helpers, "}\n");
    buf_free(&tmp);
    ce->ctx->closure = saved_lh_closure;
    ce->ctx->env_var_name = saved_lh_env_var;
}

/* A non-tail cps->cps call `let x = g(args) in jbody`, represented as a
 * CT_LETCONT whose body tailcalls g threading this join.  The join continuation
 * (jbody, parameterized by x) is reified as a DK frame chained onto the enclosing
 * continuation (`cur_k`), and g is called with that chain.  When g finishes it
 * `dk_run`s its result through the chain: the join frame transforms it (runs
 * jbody, returning jbody's value) and dk_run then delivers that to `cur_k`.
 *
 * The frame is a value-transform frame (LH_PERFORM_CONT: it *returns* jbody's
 * value rather than delivering it itself), and its `next` is `cur_k` -- so the
 * enclosing continuation, including any effect handler installed above this call,
 * stays reachable in g's continuation chain.  (A reset-cont-style frame with
 * next=dk_done would hide the handler from g and abort on a perform.)  The join
 * body is zero-capture (checked by needs_heap_join), so the frame carries no env. */
static void emit_heap_join(CE *ce, const CTerm *t) {
    const CTerm *call = t->as.letcont.body;   /* CT_TAILCALL to the colored callee */
    int id = (*ce->helper_ctr)++;
    char jname[256];
    snprintf(jname, sizeof(jname), "%s_j%d", ce->fn_cn, id);
    char *xn = cvar_cname(ce, t->as.letcont.param);

    /* P6 (heap-join-over-recursion): a jbody that captures enclosing locals or
     * makes a cps->cps tail call (the recursion in `set-eq-loop`/`__cons-fmap`,
     * or a chain of colored calls each capturing the growing live set) cannot
     * ride the value-only LH_PERFORM_CONT frame (which has no `k` and no captures,
     * and whose return would be re-delivered down `next`).  Lift it as an
     * LH_RESUME_CONT resume-frame instead: the DKK_RESUME_FRAME rfn RECEIVES its
     * run-time downstream chain as the `__kont` parameter (dk_run_impl passes
     * `k->next`) and CONSUMES it (returns rfn's result, never re-processing
     * `next`), so a KK_RET delivery lowers `dk_run(__kont, v)` and a recursive
     * `f__cps(args, __kont)` threads it -- delivered exactly once.  Captures ride
     * the env; `__kont` is the runtime param, not the env, so no `__k` field.  The
     * leaner value-only case (no caps, no cps->cps tail) keeps LH_PERFORM_CONT. */
    CapSet cs;
    bool caps_ok = collect_caps(t->as.letcont.jbody, t->as.letcont.param.id, &cs);
    const CapSet *caps = (caps_ok && cs.n > 0) ? &cs : NULL;
    bool needs_kont = jbody_has_delim(t->as.letcont.jbody)
                   || jbody_has_cps_tailcall(t->as.letcont.jbody)
                   || jbody_has_perform(t->as.letcont.jbody);

    /* colored-generic-clone-join-emitted-inline: a colored mono-template callee
     * threads its CLONE's `__cps` twin, exactly as the tail arm resolves it. */
    const char *hj_clone = call->as.tailcall.fn
        ? find_mono_clone_for_call(ce->ctx, call->as.tailcall.fn, call->as.tailcall.args,
                                   call->as.tailcall.n, call->as.tailcall.call_expr)
        : NULL;
    char *fn = call->as.tailcall.fn
        ? (hj_clone && !binding_in_s(call->as.tailcall.fn) ? strdup(hj_clone)
                                                            : callee_name(call->as.tailcall.fn))
        : atom_str(ce, &call->as.tailcall.fn_atom);  /* E2c: field-load callee */
    char *argv = atoms_csv_call(ce, call->as.tailcall.args, call->as.tailcall.n);
    /* The join frame is spliced onto cur_k and threaded into the callee in tail
     * position; register it for a single-node reap at the outermost entry
     * boundary (docs/archive/cps-delimited-dk-node-leak.md). */
    char frame[720];
    if (caps || needs_kont) {
        emit_lifted(ce, jname, LH_RESUME_CONT, xn, t->as.letcont.param.ty,
                    t->as.letcont.param.type, t->as.letcont.jbody, NULL, caps);
        char *envexpr = emit_cont_env(ce, jname, caps);   /* caps-only env */
        snprintf(frame, sizeof frame,
                 "__dk_reap_node(dk_frame_resume(%s, %s, %s))", jname, envexpr, ce->cur_k);
        free(envexpr);
    } else {
        emit_lifted(ce, jname, LH_PERFORM_CONT, xn, t->as.letcont.param.ty,
                    t->as.letcont.param.type, t->as.letcont.jbody, NULL, NULL);
        snprintf(frame, sizeof frame, "__dk_reap_node(dk_frame(%s, 0, %s))", jname, ce->cur_k);
    }
    free(xn);

    if (call->as.tailcall.via_fncps) {
        /* E2 (fat-closure fn-value threading), heap join: thread the reified join
         * `frame` to the closure's `fn_cps` slot (an effectful fn-value), or the
         * direct `f.fn` call delivered to `frame` (a pure one). */
        char *pf = name_for_binding(ce->ctx, call->as.tailcall.fn);
        uint32_t na = call->as.tailcall.n;
        char **av = fncps_args_str(ce, call->as.tailcall.args, na);
        Buf sc; buf_init(&sc);
        fncps_slot_call(&sc, pf, av, na, frame);
        buf_putc(&sc, '\0');
        ce_line(ce, "if (%s.fn_cps) return %s; /* E2 threaded fat fn-value heap join */",
                pf, sc.data);
        buf_free(&sc);
        Buf dc; buf_init(&dc);
        fncps_direct_call(&dc, pf, av, na, call->as.tailcall.call_expr);
        buf_putc(&dc, '\0');
        ce_line(ce, "return dk_run(%s, (intptr_t)(%s));", frame, dc.data);
        buf_free(&dc);
        free(pf); fncps_args_free(av, na);
    } else if (call->as.tailcall.via_registry) {
        /* E2a tier-`nontail`: the callee is a fn-value param; thread the reified
         * join `frame` to its CPS entry recovered from the registry. */
        /* Carrier ABI: pointer-like args must be int64-cast (gcc14-int-conversion). */
        char *argv_cps = atoms_csv_call_cps(ce, call->as.tailcall.args, call->as.tailcall.n);
        const Type *e2sig = e2a_callee_sig(call->as.tailcall.fn,
                                           &call->as.tailcall.fn_atom,
                                           call->as.tailcall.n);
        if (e2a_call_is_fat(call->as.tailcall.fn, &call->as.tailcall.fn_atom)) {
            emit_e2a_fat_dispatch(ce, fn, fn, argv_cps, call->as.tailcall.n,
                                  frame, "E2a threaded fn-value heap join", e2sig);
            free(argv_cps);
        } else {
        char cast[512];
        e2a_cast(cast, sizeof cast, "", e2sig, call->as.tailcall.n);
        char key[640];
        e2a_lookup_key(key, sizeof key, call->as.tailcall.fn, fn);
        if (call->as.tailcall.n)
            ce_line(ce, "return ((%s)__tur_cps_lookup_checked(%s, \"%s\"))(%s, %s); /* E2a threaded fn-value heap join */",
                    cast, key, fn, argv_cps, frame);
        else
            ce_line(ce, "return ((%s)__tur_cps_lookup_checked(%s, \"%s\"))(%s); /* E2a threaded fn-value heap join */",
                    cast, key, fn, frame);
        free(argv_cps);
        }
    } else if (call->as.tailcall.n)
    {
        /* Param-type-aware carrier casts (gcc14-int-conversion). */
        char *argv_t = atoms_csv_call_typed(ce, call->as.tailcall.args,
                                            call->as.tailcall.n, call->as.tailcall.fn,
                                            find_spec_by_clone_name(ce->ctx, fn));
        ce_line(ce, "return %s__cps(%s, %s); /* cps->cps heap join */", fn, argv_t, frame);
        free(argv_t);
    }
    else
        ce_line(ce, "return %s__cps(%s); /* cps->cps heap join */", fn, frame);
    free(fn); free(argv);
}

/* ---- cps-while-native: CT_LOOP / CT_CONTINUE ------------------------------ *
 * A CT_LOOP emits a synthesized tail-recursive colored helper
 * `<fn>_loop<id>__cps(<params>, DK *__kont)` at file scope (into ce->helpers,
 * like emit_lifted's helpers) whose body is the loop's CT_IF; a CT_CONTINUE
 * back-edge re-enters it.  The helper is FORWARD-declared first so both its own
 * self-recursion and the interior handle's lifted continuation (which carries the
 * back-edge) can call it before its definition appears in the buffer.  At the loop
 * site, the caller emits `return <helper>__cps(<inits>, <thread>)`. */
/* perform-inside-loop-has-no-lowering: a join that a LIFTED region of its body
 * delivers to -- `letcont j(x) = rest in if c then (perform ...; j unit) else
 * (j unit)`, the statement-position conditional perform of every "if
 * collision, perform GameOver" loop body.  The perform continuation is its own
 * C function, so it can neither assign the join slot nor `goto L<j>` (it used
 * to emit exactly that: `0 = __t5; goto L4;`).  Reify the join as an
 * LH_RESUME_CONT frame spliced onto cur_k and make it cur_k while the body
 * emits: a perform inside splices its continuation onto the join node, so the
 * continuation's downstream delivery (KK_RET or a `(j v)` seen from a lifted
 * frame, emit_deliver_ty) lands in the join body; the parent frame's own
 * `(j v)` runs the node directly.  The join body delivers through the node's
 * next -- the cur_k it was created under -- exactly once. */
static void emit_escaping_join(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char jname[256];
    snprintf(jname, sizeof(jname), "%s_ej%d", ce->fn_cn, id);
    char *xn = cvar_cname(ce, t->as.letcont.param);
    CapSet cs;
    bool caps_ok = collect_caps(t->as.letcont.jbody, t->as.letcont.param.id, &cs);
    const CapSet *caps = (caps_ok && cs.n > 0) ? &cs : NULL;
    emit_lifted(ce, jname, LH_RESUME_CONT, xn, t->as.letcont.param.ty,
                t->as.letcont.param.type, t->as.letcont.jbody, NULL, caps);
    free(xn);
    char *envexpr = emit_cont_env(ce, jname, caps);   /* caps-only env */
    char fv[24];
    snprintf(fv, sizeof fv, "__ej%d", id);
    /* Single spliced node, reaped at the outermost entry boundary like every
     * other structural node (docs/archive/cps-delimited-dk-node-leak.md). */
    ce_line(ce, "DK *%s = __dk_reap_node(dk_frame_resume(%s, %s, %s));",
            fv, jname, envexpr, ce->cur_k);
    free(envexpr);
    if (ce->n_hjoins >= MAX_JOINS) {
        /* Table full: fall back to a plain label join (the term was admitted,
         * so this is a capacity limit, not a shape). */
        emit_term(ce, t->as.letcont.body);
        return;
    }
    int slot = ce->n_hjoins++;
    ce->hjoins[slot].id = t->as.letcont.j.id;
    snprintf(ce->hjoins[slot].frame, sizeof ce->hjoins[slot].frame, "%s", fv);
    ce->hjoins[slot].out = ce->out;
    const char *saved_k = ce->cur_k;
    ce->cur_k = ce->hjoins[slot].frame;   /* lives while the body emits */
    emit_term(ce, t->as.letcont.body);
    ce->cur_k = saved_k;
    ce->n_hjoins--;
}

static void emit_loop(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char lname[256];
    snprintf(lname, sizeof(lname), "%s_loop%d", ce->fn_cn, id);
    uint32_t np = t->as.loop.n_params;

    /* cps-while-native: collect the loop body's loop-INVARIANT free vars -- source
     * vars/CVars it reads that are NOT loop-carried params (e.g. a fn param or a
     * handle result a loop in a handle continuation reads).  They thread as extra
     * helper params, passed unchanged on every back-edge.  A collect bail
     * (cs->ok=false) falls back to no-extras (prior behaviour). */
    CapSet inv; inv.n = 0; inv.ok = true;
    {
        uint32_t bnd[CC_MAX_BOUND]; uint32_t nbnd = 0;
        for (uint32_t i = 0; i < np && nbnd < CC_MAX_BOUND; i++)
            if (t->as.loop.params[i].bind) bnd[nbnd++] = t->as.loop.params[i].bind->id;
        collect_caps_rec(t->as.loop.body, UINT32_MAX, bnd, nbnd, &inv);
        if (!inv.ok) inv.n = 0;
    }
    /* Shared name CSV: helper params, entry-call args, and back-edge args all name
     * the invariants identically (name_for_binding / cvname), since each is in
     * scope at the entry site and a param inside the helper. */
    char *inv_names = NULL;
    if (inv.n > 0) {
        Buf nbuf; buf_init(&nbuf);
        for (int i = 0; i < inv.n; i++) {
            char *cn = inv.b[i] ? name_for_binding(ce->ctx, inv.b[i]) : strdup(inv.cvname[i]);
            buf_printf(&nbuf, "%s%s", i ? ", " : "", cn);
            free(cn);
        }
        buf_putc(&nbuf, '\0');
        inv_names = strdup(nbuf.data);
        buf_free(&nbuf);
    }

    /* 1. Forward declaration (before nested helpers reference it). */
    buf_printf(ce->helpers, "static int64_t %s__cps(", lname);
    for (uint32_t i = 0; i < np; i++)
        buf_printf(ce->helpers, "%s, ",
                   binder_ctype_full(ce->ctx, t->as.loop.params[i].ty, t->as.loop.params[i].type));
    for (int i = 0; i < inv.n; i++)
        buf_printf(ce->helpers, "%s, ", cap_ctype(ce->ctx, &inv, i));
    buf_puts(ce->helpers, "DK *);\n");

    /* 2. Emit the loop body into a temp buffer; nested handle/reset helpers
     * accumulate into ce->helpers ahead of the loop helper definition. */
    Buf tmp; buf_init(&tmp);
    /* Where does the loop's exit value go?  Its exit arm delivers to the
     * helper's own KK_RET; what THAT means depends on how the enclosing frame
     * delivers to the loop's result_kont.  In an ordinary function or a
     * reset/handle continuation the target is a real DK chain -- thread it and
     * dk_run at the exit (ret_direct=false, the historical behavior).  In a
     * handler-case / shift-body frame a KK_PROMPT delivery is a plain
     * `return` (shift_mode), and in a perform-continuation frame a KK_RET
     * delivery is too (ret_mode) -- there is NO chain to thread, so the
     * helper must RETURN its exit value and be entered with a NULL kont it
     * never reads.  This is what lets a `while` live inside a handler clause:
     * the case helper returns the loop helper's return, and dk_perform routes
     * it exactly as it routes a straight-line case value. */
    /* A KK_VAR result kont is an ESCAPING join (the loop is followed by more
     * statements, reified as a resume-frame by emit_escaping_join): thread
     * that frame as the helper's kont; the exit's KK_RET delivery runs it. */
    const char *join_frame = NULL;
    if (t->as.loop.result_kont.kind == KK_VAR) {
        for (int i = ce->n_hjoins - 1; i >= 0; i--)
            if (ce->hjoins[i].id == t->as.loop.result_kont.id) { join_frame = ce->hjoins[i].frame; break; }
    }
    bool ret_direct = (t->as.loop.result_kont.kind == KK_PROMPT)
                          ? ce->shift_mode
                          : (t->as.loop.result_kont.kind == KK_RET ? ce->ret_mode : false);
    CE hc = *ce;
    hc.out = &tmp;
    hc.indent = 4;
    hc.n_joins = 0;
    hc.cur_k = "__kont";
    hc.cur_loop_name = lname;
    hc.cur_loop_inv = inv_names;
    hc.shift_mode = false;
    hc.ret_mode = ret_direct;
    hc.handler_case_mode = false;
    hc.case_tail_resume = false;
    /* The exit arm delivers the live-after var to the helper's KK_RET; its
     * crossing type matches what the caller's continuation expects. */
    hc.ret_ty = (t->as.loop.result_kont.kind == KK_PROMPT) ? ce->cur_ty
              : (t->as.loop.result_kont.kind == KK_RET ? ce->ret_ty : NULL);
    /* Frames lifted out of the body that take the back-edge need the
     * invariants in their env (collect_caps merges g_loop_inv). */
    const CapSet *saved_inv = g_loop_inv;
    g_loop_inv = (inv.n > 0) ? &inv : NULL;
    emit_binder_decls(&hc, t->as.loop.body);
    emit_term(&hc, t->as.loop.body);
    g_loop_inv = saved_inv;
    buf_putc(&tmp, '\0');

    /* 3. Emit the helper definition. */
    buf_printf(ce->helpers, "static int64_t %s__cps(", lname);
    for (uint32_t i = 0; i < np; i++) {
        char *pn = t->as.loop.params[i].bind
            ? name_for_binding(ce->ctx, t->as.loop.params[i].bind)
            : strdup(t->as.loop.params[i].name);
        buf_printf(ce->helpers, "%s %s, ",
                   binder_ctype_full(ce->ctx, t->as.loop.params[i].ty, t->as.loop.params[i].type), pn);
        free(pn);
    }
    for (int i = 0; i < inv.n; i++) {
        char *cn = inv.b[i] ? name_for_binding(ce->ctx, inv.b[i]) : strdup(inv.cvname[i]);
        buf_printf(ce->helpers, "%s %s, ", cap_ctype(ce->ctx, &inv, i), cn);
        free(cn);
    }
    buf_puts(ce->helpers, "DK *__kont) {\n");
    buf_puts(ce->helpers, tmp.data);
    buf_puts(ce->helpers, "}\n");
    buf_free(&tmp);

    /* 4. The loop entry: tail-call the helper with the entry values (+ invariants),
     * threading the enclosing continuation (KK_RET -> this fn's __kont, KK_PROMPT
     * -> cur_k). */
    char *argv = atoms_csv(ce, t->as.loop.inits, np);
    /* ret_direct: the helper returns its exit value and never reads its kont
     * param -- see the mode note above. */
    const char *thread = ret_direct ? "NULL"
        : (t->as.loop.result_kont.kind == KK_PROMPT) ? ce->cur_k
        : join_frame ? join_frame : "__kont";
    if (np && inv_names)
        ce_line(ce, "return %s__cps(%s, %s, %s); /* cps-while-native loop entry */", lname, argv, inv_names, thread);
    else if (np)
        ce_line(ce, "return %s__cps(%s, %s); /* cps-while-native loop entry */", lname, argv, thread);
    else if (inv_names)
        ce_line(ce, "return %s__cps(%s, %s); /* cps-while-native loop entry */", lname, inv_names, thread);
    else
        ce_line(ce, "return %s__cps(%s); /* cps-while-native loop entry */", lname, thread);
    free(inv_names);
    free(argv);
}

/* The loop back-edge: re-enter the enclosing loop helper with the next-iteration
 * values (the $next CVars a set! bound), threading this frame's own continuation
 * (__kont -- the loop helper's k, reachable inside a lifted handle continuation
 * as env->__k). */
static void emit_continue(CE *ce, const CTerm *t) {
    char *argv = atoms_csv(ce, t->as.cont_.args, t->as.cont_.n);
    const char *inv = ce->cur_loop_inv;   /* loop-invariant extra args, unchanged */
    const char *lname = ce->cur_loop_name ? ce->cur_loop_name : "0";
    if (t->as.cont_.n && inv)
        ce_line(ce, "return %s__cps(%s, %s, __kont); /* cps-while-native back-edge */", lname, argv, inv);
    else if (t->as.cont_.n)
        ce_line(ce, "return %s__cps(%s, __kont); /* cps-while-native back-edge */", lname, argv);
    else if (inv)
        ce_line(ce, "return %s__cps(%s, __kont); /* cps-while-native back-edge */", lname, inv);
    else
        ce_line(ce, "return %s__cps(__kont); /* cps-while-native back-edge */", lname);
    free(argv);
}

/* N6.3: emit the alloc+populate of a lifted continuation's env -- the body's
 * scalar captures, nothing else.  Returns a malloc'd C expr for the DK frame
 * env: the env struct pointer when there are captures, else "0".  The struct
 * is leaked with the DK nodes (reaped at the entry boundary). */
static char *emit_cont_env(CE *ce, const char *hname, const CapSet *caps) {
    /* The env is caps-only: every lifted frame that needs its enclosing
     * continuation receives it AT RUN TIME (a DKResumeFrame's third parameter,
     * from the node's own ->next).  The pre-unification `__k` env slot -- a
     * pointer to the ORIGINAL chain baked in at install time, which every
     * dk_copy_range copy still jumped to -- is gone, and this signature keeps
     * it gone (docs/archive/cps-multishot-nontail-resume-inner-handle-drops-clause-rest.md). */
    if (!caps || caps->n == 0) return strdup("0");
    char envv[300];
    snprintf(envv, sizeof envv, "__ce_%s", hname);
    ce_line(ce, "%s_env *%s = (%s_env *)malloc(sizeof(%s_env));", hname, envv, hname, hname);
    for (int i = 0; i < caps->n; i++) {
        char *cn = caps->b[i] ? name_for_binding(ce->ctx, caps->b[i]) : strdup(caps->cvname[i]);
        ce_line(ce, "%s->f%d = %s;", envv, i, cn);
        free(cn);
    }
    /* The env struct outlives its DK frame (shared read-only across multi-shot
     * resumes) and is never freed at a call site; reap it at the outermost entry
     * boundary (docs/archive/cps-delimited-dk-node-leak.md). */
    ce_line(ce, "__dk_reap_ptr((intptr_t)%s);", envv);
    Buf b; buf_init(&b); buf_printf(&b, "(intptr_t)%s", envv); buf_putc(&b, '\0');
    char *s = strdup(b.data); buf_free(&b); return s;
}

/* CT_RESET: install a prompt whose outer continuation is the lifted reset body
 * (carrying k + captures), then emit the delimited body threading that prompt
 * chain.  The per-reset DK nodes are leaked (DK is opaque; see
 * docs/archive/cps-delimited-dk-node-leak.md), matching the abortive path. */
static void emit_reset(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char hname[256];
    snprintf(hname, sizeof(hname), "%s_k%d", ce->fn_cn, id);
    CapSet cs;
    bool ok = collect_caps(t->as.reset.body, t->as.reset.x.id, &cs);
    const CapSet *caps = (ok && cs.n > 0) ? &cs : NULL;
    char *rxn = cvar_cname(ce, t->as.reset.x);
    emit_lifted(ce, hname, LH_RESUME_CONT, rxn, t->as.reset.x.ty, t->as.reset.x.type,
                t->as.reset.body, NULL, caps);
    free(rxn);

    char *envexpr = emit_cont_env(ce, hname, caps);   /* caps-only env */
    char pchain[64];
    snprintf(pchain, sizeof(pchain), "__p%d", id);
    /* The reset continuation frame is a RESUME-FRAME whose `next` is the ACTUAL
     * enclosing chain, borrowed -- the same spine unification the handle frame
     * got (docs/archive/cps-reset-frame-pre-unification-layout.md, following
     * docs/archive/cps-multishot-nontail-resume-inner-handle-drops-clause-rest.md).
     * One spine serves everything the old layout split: an effect PERFORMED
     * inside the delimited body (handled by a handler ENCLOSING the reset)
     * walks through the prompt straight into the real enclosing handlers, its
     * capture crosses into the real intermediate frames, and its delivery runs
     * the real rest -- where the old baked-`__k`-env frame with marker-copy
     * `next` truncated the capture and dead-ended delivery.  A `shift` is
     * unaffected: its capture stops at the prompt, BEFORE this frame.  The
     * chain (prompt + frame) is installed in tail position, so it cannot be
     * freed here; register it for reaping at the outermost entry boundary
     * (docs/archive/cps-delimited-dk-node-leak.md) -- borrow_next keeps that
     * dk_free out of the enclosing chain. */
    ce_line(ce, "DK *%s = __dk_reap_keep(dk_prompt(1, dk_frame_resume_borrow(%s, %s, %s)));",
            pchain, hname, envexpr, ce->cur_k);
    free(envexpr);

    const char *save = ce->cur_k;
    const Type *save_ty = ce->cur_ty;
    ce->cur_k = arena_strdup(&g_arena, pchain, strlen(pchain));
    ce->cur_ty = t->as.reset.x.type;    /* KK_PROMPT crossing type = reset result */
    emit_term(ce, t->as.reset.delim);   /* delivers to the prompt chain, tail */
    ce->cur_k = save;
    ce->cur_ty = save_ty;
}

/* The C body expression of a single arithmetic context frame `(<op> <operand>
 * [])` -- `env` is the captured operand, `value` the resumed value.  Mirrors
 * emit_cps.c's frame_c_expr byte-for-byte (the proven generator). */
/* The C cast a cloneable/serial continuation thunk applies when forwarding its
 * intptr-carried `env`/`value` slot into a top-level callee's parameter, keyed
 * by the target's param kind.  Mirrors emit_cps.c's c_cast_for_kind.  A callee
 * param that lowers to a POINTER type (rc/weak -> RcControlBlock *, ref/lref/
 * ptr -> void *, cstr -> const char *) must be cast to that pointer type, not
 * to (int64_t): passing an int where a pointer is expected is a
 * -Wint-conversion (benign on LP64, but not -Werror-clean).  A (void *) cast
 * covers every object-pointer param -- void * converts to any object pointer
 * implicitly in C -- so a typed `ptr<T>` param (`T *`) is satisfied too.
 * Everything else keeps the int64_t carrier cast (a cstr rides the carrier like
 * an int in the CC4 value-typed cont<cstr> path).  See
 * docs/archive/cloneable-cont-thunk-borrow-rc-int-conversion.md. */
static const char *cc_cast_for_kind(TypeKind k) {
    switch (k) {
        case TY_CSTR:           return "(const char *)";
        case TY_RC:
        case TY_WEAK:           return "(RcControlBlock *)";
        case TY_REF:
        case TY_LREF:
        case TY_REF_MUT:
        case TY_PTR_VOID:       return "(void *)";
        case TY_REF_IMMUT:      return "(const void *)";
        case TY_CLONEABLE_CONT: return "(tur_cloneable_cont *)";
        default:                return "(int64_t)";
    }
}

/* gcc14-int-conversion (cloneable-frame call arg -> concrete-pointer param): a
 * resumed cloneable-frame arg rides the `intptr_t` slot and is cast to the
 * callee's param C type before the call.  cc_cast_for_kind maps every nominal
 * ADT/opaque kind to the generic `(int64_t)` carrier cast, but the callee's
 * ACTUAL emitted param can be a concrete pointer (`tur_adt_H *`) -- and
 * `(int64_t)` into a pointer param is a -Wint-conversion straddle (a hard error
 * under macOS clang / GCC >= 14).  When the callee's full param type resolves to
 * a pointer C type, cast to THAT (an int->ptr cast from the intptr_t slot is
 * value-preserving); otherwise keep the kind-based carrier cast (which already
 * handles the cstr, void-pointer and rc cases correctly).  Writes the cast
 * (e.g. "(tur_adt_H *)") into `out`. */
static void cc_cast_for_param(CE *ce, const Binding *cfn, uint32_t i,
                              char *out, size_t outsz) {
    TypeKind k = (cfn && cfn->type.kind == TY_FN && i < cfn->type.as.fn.arity)
        ? cfn->type.as.fn.arg_kinds[i] : TY_INT;
    const char *kc = cc_cast_for_kind(k);
    /* Only the default int64 carrier cast can straddle a concrete-pointer param;
     * the explicit pointer casts (cstr, void-pointer, rc) are already correct. */
    if (strcmp(kc, "(int64_t)") == 0 && cfn && cfn->type.kind == TY_FN &&
        i < cfn->type.as.fn.arity && cfn->type.as.fn.arg_full_types) {
        const Type *ft = cfn->type.as.fn.arg_full_types[i];
        const char *ct = ft ? binder_ctype_full(ce->ctx, ft->kind, ft) : NULL;
        size_t L = ct ? strlen(ct) : 0;
        if (ct && L >= 1 && ct[L - 1] == '*') {
            snprintf(out, outsz, "(%s)", ct);
            return;
        }
    }
    snprintf(out, outsz, "%s", kc);
}

static const char *cloneable_frame_expr(const char *op, bool hole_left) {
    if (strcmp(op, "+") == 0) return "env + value";
    if (strcmp(op, "*") == 0) return "env * value";
    if (strcmp(op, "-") == 0) return hole_left ? "value - env" : "env - value";
    if (hole_left)  /* "/" hole-left: value / env */
        return "(env) ? (value / env) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0)";
    return "(value) ? (env / value) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0)";
}

/* U4 serial: the stable marshaler tag for an arithmetic context frame (op + hole
 * side), mirroring emit_cps.c's sk_tag_for_frame.  The serial runtime prelude
 * emits `__sk_frame_for_tag(tag)` returning the matching DKFrame, and the
 * marshaler maps frame <-> tag by this table so save-cont!/resume-cont! round-
 * trips.  1=ADD 2=MUL 3=SUBR(env-v) 4=SUBL(v-env) 5=DIVR(env/v) 6=DIVL(v/env). */
static int sk_tag_for_frame(const CloneFrame *fr) {
    if (strcmp(fr->op, "+") == 0) return 1;
    if (strcmp(fr->op, "*") == 0) return 2;
    if (strcmp(fr->op, "-") == 0) return fr->hole_left ? 4 : 3;
    return fr->hole_left ? 6 : 5;   /* "/" */
}

/* Resolve the Serializable instance's serialize/deserialize C names for a nominal
 * (TY_ADT) serial call-frame env type `t`.  Returns malloc'd names via out-params
 * and true, or false if no instance.  Mirrors emit_cps.c's sk_find_serializable
 * name path, kept in the native path so the CT-IR serial emitter owns its env
 * marshaling (the runtime Sk registry already encodes SK_ENV_SER). */
/* fnsan-serial-registry-hooks: the C types of the instance's methods, so the
 * registry can hold adapters at its own fixed slot types (`void *(*)(int64_t)`,
 * `int64_t (*)(void *)`) instead of the methods cast to them -- an indirect
 * call through the wrong function type (a -fsanitize=function trap, a
 * call_indirect trap on WASM) for every env type that is not an int64. */
typedef struct SerSigs { const char *ser_p, *ser_r, *deser_p, *deser_r; } SerSigs;
static SerSigs g_serial_env_sigs;
static Type ser_sig_arg0(Type ft) {
    if (ft.as.fn.arg_full_types && ft.as.fn.arg_full_types[0])
        return *ft.as.fn.arg_full_types[0];
    return emit_type_from_kind(ft.as.fn.arg_kinds[0]);
}
static Type ser_sig_result(Type ft) {
    if (ft.as.fn.result_full_type) return *ft.as.fn.result_full_type;
    return emit_type_from_kind(ft.as.fn.result_kind);
}

static bool serial_env_ser_names(CE *ce, const Type *t,
                                 char **ser_out, char **deser_out) {
    const Expr *program = ce->ctx->program_root;
    if (!program || program->kind != EX_PROGRAM) return false;
    if (!t || t->kind != TY_ADT || !t->as.adt_.def || !t->as.adt_.def->name) return false;
    const char *tname = t->as.adt_.def->name;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        const Expr *it = program->as.program.items[i];
        if (!it || it->kind != EX_INSTANCE_DEF) continue;
        const TypeClassInstance *inst = it->as.instance_def_.instance;
        if (!inst || !inst->typeclass || !inst->typeclass->name) continue;
        if (strcmp(inst->typeclass->name->name, "Serializable") != 0) continue;
        if (inst->n_type_args < 1) continue;
        Type at = inst->type_args[0];
        if (at.kind != TY_ADT || !at.as.adt_.def || !at.as.adt_.def->name) continue;
        if (strcmp(at.as.adt_.def->name, tname) != 0) continue;
        char *ser = NULL, *deser = NULL;
        const TypeClass *tc = inst->typeclass;
        for (uint8_t j = 0; j < tc->n_methods && j < inst->n_method_impls; j++) {
            if (!inst->method_impls[j] || !inst->method_impls[j]->binding) continue;
            const char *mn = tc->methods[j].name ? tc->methods[j].name->name : "";
            char *cn = raw_name_for_binding(inst->method_impls[j]->binding);
            /* The binding's type keeps the class variable; the parameter
             * itself carries the instance's type, which is what the method is
             * emitted at. */
            const FnDef *mfd = inst->method_impls[j];
            Type mt = mfd->binding->type;
            const char *pc = NULL;
            if (mfd->n_params >= 1 && mfd->params && mfd->params[0])
                pc = emit_type_c_name(ce->ctx, emit_resolve_type(ce->ctx,
                         mfd->param_types ? mfd->param_types[0] : mfd->params[0]->type));
            else if (mt.kind == TY_FN && mt.as.fn.arity >= 1)
                pc = emit_type_c_name(ce->ctx, emit_resolve_type(ce->ctx,
                         ser_sig_arg0(mt)));
            const char *rc = mt.kind == TY_FN
                ? emit_type_c_name(ce->ctx, emit_resolve_type(ce->ctx,
                      ser_sig_result(mt)))
                : NULL;
            if (strcmp(mn, "serialize") == 0) {
                ser = cn;
                g_serial_env_sigs.ser_p = pc; g_serial_env_sigs.ser_r = rc;
            } else if (strcmp(mn, "deserialize") == 0) {
                deser = cn;
                g_serial_env_sigs.deser_p = pc; g_serial_env_sigs.deser_r = rc;
            } else free(cn);
        }
        if (ser && deser) { *ser_out = ser; *deser_out = deser; return true; }
        free(ser); free(deser);
        return false;
    }
    return false;
}

/* CT_CLONEABLE (U3): (cloneable-reset (cloneable-shift receiver val)) and its
 * single-arithmetic-frame Shape 2 form.  Emits the cloneable multi-shot machinery
 * natively (no emit_cps.c), reusing the shared DK runtime (dk_copy_range,
 * __dk_cont_fn / __dk_env_clone / __dk_env_drop) byte-for-byte.
 *
 *   Shape 1 (n_frames == 0): identity continuation -- no dk_copy_range.  Alloc a
 *     tur_cloneable_cont over an identity fn (NULL env), call receiver, bind x.
 *   Shape 2 (n_frames >= 1): reify the `(<op> <operand> ... [])` context as DK
 *     frame; the shift body deep-clones the captured sub-continuation into a
 *     cloneable_cont and hands it to receiver.  Mirrors emit_cloneable_ctx. */
/* Direct-emit a self-contained (shift-free) context Expr -- a `let` init, an
 * `if` condition, or an `if` pure arm -- at the reset site, honoring the CT
 * emitter's current indent.  Returns the malloc'd C expression string. */
static char *emit_cloneable_direct(CE *ce, const Expr *e) {
    int saved = ce->ctx->indent;
    ce->ctx->indent = ce->indent;
    char *v = emit_value(ce->ctx, ce->out, e);
    ce->ctx->indent = saved;
    return v;
}

/* Build the pure-arm value of an if-split cloneable/serial context: the direct
 * value of `if_pure` with the OUTER context frames re-applied (frames[0..
 * n_outer_frames), applied innermost-outer first).  The shift-bearing arm rides
 * the full frame chain through the DK machine; the pure arm carries no shift, so
 * its outer frames must be applied here as plain C.  Inner frames
 * (frames[n_outer_frames..n_frames)) live inside the shift arm and do NOT apply
 * to the pure arm.  Mirrors cloneable_frame_expr's per-op semantics (env =
 * operand, value = the hole).  Returns a malloc'd C expression. */
static char *emit_cloneable_pure_arm(CE *ce, const CTerm *t) {
    char *acc = emit_cloneable_direct(ce, t->as.cloneable.if_pure);
    for (int i = (int)t->as.cloneable.n_outer_frames - 1; i >= 0; i--) {
        const CloneFrame *fr = &t->as.cloneable.frames[i];
        Buf b; buf_init(&b);
        if (fr->call_fn) {
            char *cfn = callee_name(fr->call_fn);
            if (!fr->ignore_value && fr->call_fn->type.as.fn.arity == 2) {
                /* 2-arg outer call frame: apply f to (pure, env) in source order
                 * (hole side = the pure value; the other arg is the captured env). */
                char *opv = atom_str(ce, &fr->operand);
                if (fr->hole_left)
                    buf_printf(&b, "((int64_t)%s((int64_t)(%s), (int64_t)(%s)))", cfn, acc, opv);
                else
                    buf_printf(&b, "((int64_t)%s((int64_t)(%s), (int64_t)(%s)))", cfn, opv, acc);
                free(opv);
            } else {
                /* 1-arg outer call frame: apply f to the pure value. */
                buf_printf(&b, "((int64_t)%s((int64_t)(%s)))", cfn, acc);
            }
            free(cfn);
        } else {
            char *opv = atom_str(ce, &fr->operand);
            const char *op = fr->op;
            if (strcmp(op, "+") == 0)
                buf_printf(&b, "((%s) + (%s))", opv, acc);
            else if (strcmp(op, "*") == 0)
                buf_printf(&b, "((%s) * (%s))", opv, acc);
            else if (strcmp(op, "-") == 0)
                buf_printf(&b, fr->hole_left ? "((%s) - (%s))" : "((%s) - (%s))",
                           fr->hole_left ? acc : opv, fr->hole_left ? opv : acc);
            else /* "/" -- guard div-by-zero exactly like cloneable_frame_expr */
                buf_printf(&b,
                    "((%s) ? ((%s) / (%s)) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0))",
                    fr->hole_left ? opv : acc,
                    fr->hole_left ? acc : opv,
                    fr->hole_left ? opv : acc);
            free(opv);
        }
        char *nv = strdup(b.data);
        buf_free(&b);
        free(acc);
        acc = nv;
    }
    return acc;
}

/* Emit the Shape 2 shift-body helper `bodyfn`.  `cont_setup` is the C that builds
 * the continuation the receiver is handed (a raw DK chain for serial, a
 * cloneable_cont for cloneable); `cont_arg` names it.  For a named-fn receiver the
 * dk_shift env carries the fn ptr and the body calls it as one; for a CLOSURE
 * receiver (receiver_expr) the closure's thunk is baked in here and the dk_shift
 * env carries the closure env instead (see emit_cl_shift_env). */
/* The C spelling of a SERIAL receiver's continuation parameter.  Both
 * spellings the language offers for it reach here: `k : ptr<void>` (the
 * workflow.tur convention) lowers to `void *`, while the typed
 * `k : serial-cont` -- `cont<int>`, what stdlib/serial.tur and the guides use
 * -- lowers to the int64 carrier.  The DK chain is handed over in whichever
 * the receiver declares; keying on the family alone passed a pointer to an
 * int64_t param (-Wint-conversion, a hard error on GCC >= 14 and macOS clang)
 * for a closure receiver, and called a named one through a mismatched
 * function-pointer type.  Found while admitting colored receivers
 * (serial-shift-colored-receiver-rejected), which made the typed spelling
 * reachable from more shapes. */
static const char *serial_recv_kty(CE *ce, const CTerm *t) {
    TypeKind pk = TY_PTR_VOID;
    const Type *pt = NULL;
    if (t->as.cloneable.receiver_expr) {
        /* The receiver's one parameter is its LAST: a capturing closure's
         * FnDef has the synthesized `__env_p_<id>` env param prepended. */
        const struct Closure *cl = t->as.cloneable.receiver_expr->as.closure_.closure;
        if (cl && cl->fn && cl->fn->n_params > 0 && cl->fn->params[cl->fn->n_params - 1]) {
            pt = &cl->fn->params[cl->fn->n_params - 1]->type;
            pk = pt->kind;
        }
    } else if (t->as.cloneable.receiver
               && t->as.cloneable.receiver->type.kind == TY_FN
               && t->as.cloneable.receiver->type.as.fn.arity >= 1) {
        const Type *ft = &t->as.cloneable.receiver->type;
        pk = ft->as.fn.arg_kinds[0];
        pt = ft->as.fn.arg_full_types ? ft->as.fn.arg_full_types[0] : NULL;
    }
    const char *ct = binder_ctype_full(ce->ctx, pk, pt);
    size_t L = ct ? strlen(ct) : 0;
    return (L && ct[L - 1] == '*') ? "void *" : "int64_t";
}

/* serial-cont-chain-never-freed: does the serial-shift receiver keep its
 * continuation `k` to itself?  A receiver is handed a DK chain it owns, and
 * `(k v)` resumes a COPY of it (dk_invoke), as does every other reader below,
 * so once the receiver has returned, a `k` it only resumed, serialized or
 * marshalled is dead -- and was never freed.  Conservative: `k` stored,
 * returned, or passed anywhere else keeps the chain alive (the old
 * behaviour); a lambda capturing it is followed only when that lambda is
 * itself only called (serial_k_closure_body_ok).  A reader is `(k v)` itself (the
 * tur_serial_cont_resume builtin), a Turmeric-bodied callee that in turn
 * confines its parameter (`serial-resume`, `workflow-suspend`), or one of the
 * stdlib's inline-C marshalers (`serial-cont->bytes`, `save-cont!`), each of
 * which copies.  See docs/archive/serial-cont-chain-never-freed.md. */
static bool serial_k_mentions(const Expr *x, const Binding *k) {
    if (!x) return false;
    uint32_t n = 0;
    Binding **fv = collect_free_vars(x, NULL, 0, NULL, 0, &n);
    bool hit = false;
    for (uint32_t i = 0; i < n && !hit; i++) hit = (fv[i] == k);
    free(fv);
    return hit;
}

static bool serial_k_is(const Expr *x, const Binding *k) {
    while (x && x->kind == EX_ASCRIBE) x = x->as.ascribe_.inner;
    return x && x->kind == EX_VAR && x->as.var.binding == k;
}

/* What the walk below tracks.  K_SERIAL: the receiver's serial-cont, whose
 * readers copy it.  K_FNPARAM: a fn value (a lambda that captured the
 * serial-cont) that may only be CALLED -- never stored, returned, captured or
 * passed on -- so it is dead when its holder returns. */
typedef enum { K_SERIAL, K_FNPARAM } SerialKRole;

static bool serial_k_use_ok(const Expr *x, const Binding *k, SerialKRole role,
                            int depth);

/* Does callee `cfd` confine its parameter `i` in `role`?  A Turmeric body is
 * walked like the receiver's own, a few calls deep.  An inline-C body is
 * opaque, so only the stdlib's own serial-cont readers are trusted -- and only
 * when they really are the stdlib's (a user defn of the same name could store
 * `k`). */
static bool serial_k_param_confined(const FnDef *cfd, uint32_t i,
                                    SerialKRole role, int depth) {
    if (!cfd || !cfd->body || cfd->is_variadic || i >= cfd->n_params) return false;
    if (cfd->body->kind == EX_INLINE_C ||
        (cfd->binding && cfd->binding->body_is_inline_c)) {
        const char *nm = (cfd->binding && cfd->binding->name)
                         ? cfd->binding->name->name : NULL;
        const char *path = diag_file_path(cfd->body->span.file_id);
        return role == K_SERIAL && i == 0 && nm && path && strstr(path, "stdlib/") &&
               (strcmp(nm, "serial-cont->bytes") == 0 ||
                strcmp(nm, "save-cont!") == 0);
    }
    return depth < 4 && serial_k_use_ok(cfd->body, cfd->params[i], role, depth + 1);
}

/* A closure literal that captures `k` confines it when its own body does --
 * provided the lambda itself is then only CALLED (K_FNPARAM) by whatever holds
 * it, so it, and the `k` inside it, are dead once that holder returns.  The
 * callers below check the second half: a call argument against the callee's
 * parameter, a `let`-bound lambda against the rest of the `let`. */
static bool serial_k_closure_body_ok(const Expr *arg, const Binding *k,
                                     SerialKRole role, int depth) {
    while (arg && arg->kind == EX_ASCRIBE) arg = arg->as.ascribe_.inner;
    if (!arg || arg->kind != EX_CLOSURE || !arg->as.closure_.closure) return false;
    const struct Closure *cl = arg->as.closure_.closure;
    if (!cl->fn || !cl->fn->body || depth >= 4) return false;
    /* The captured k must be read through the same Binding in the lambda's
     * body; a capture under any other name would hide its uses from the walk. */
    bool captured = false;
    for (uint32_t c = 0; c < cl->n_captures && !captured; c++)
        captured = (cl->captures[c] == k);
    if (!captured || !serial_k_mentions(cl->fn->body, k)) return false;
    return serial_k_use_ok(cl->fn->body, k, role, depth + 1);
}

static bool serial_k_use_ok(const Expr *x, const Binding *k, SerialKRole role,
                            int depth) {
    if (!x) return true;
    switch (x->kind) {
        case EX_VAR:
            return x->as.var.binding != k;
        case EX_ASCRIBE:
            return serial_k_use_ok(x->as.ascribe_.inner, k, role, depth);
        case EX_RESUME: {
            const ResumeExpr *r = x->as.resume_.resume;
            if (!r) return true;
            if (role == K_SERIAL && serial_k_is(r->k, k)) {
                /* a resume of k is a copying read */
            } else if (!serial_k_use_ok(r->k, k, role, depth)) {
                return false;
            }
            return serial_k_use_ok(r->value, k, role, depth);
        }
        case EX_CALL: {
            const Binding *fb = x->as.call_.fn_binding;
            if (x->as.call_.fn_expr) {
                /* `(f v)` through a fn value: calling the tracked fn is its
                 * one allowed use. */
                if (!(role == K_FNPARAM && serial_k_is(x->as.call_.fn_expr, k)) &&
                    serial_k_mentions(x->as.call_.fn_expr, k))
                    return false;
            } else if (fb == k && role != K_FNPARAM) {
                return false;
            }
            const FnDef *cfd = (fb && fb != k && g_prog) ? fd_for_binding(g_prog, fb) : NULL;
            for (uint32_t i = 0; i < x->as.call_.n_args; i++) {
                const Expr *a = x->as.call_.args[i];
                if (serial_k_is(a, k) && serial_k_param_confined(cfd, i, role, depth))
                    continue;
                if (serial_k_closure_body_ok(a, k, role, depth) &&
                    serial_k_param_confined(cfd, i, K_FNPARAM, depth))
                    continue;
                if (!serial_k_use_ok(a, k, role, depth)) return false;
            }
            return true;
        }
        case EX_BUILTIN: {
            /* `(k v)` on a serial-cont elaborates to this resume builtin
             * (elab_call's CC4 dispatch), the handle its argument 0. */
            const BuiltinSpec *sp = x->as.builtin.spec;
            bool reader = role == K_SERIAL && sp && sp->name &&
                          (strcmp(sp->name, "tur_serial_cont_resume") == 0 ||
                           strcmp(sp->name, "tur_serial_cont_serialize") == 0);
            for (uint32_t i = 0; i < x->as.builtin.n; i++) {
                if (reader && i == 0 && serial_k_is(x->as.builtin.args[i], k)) continue;
                if (!serial_k_use_ok(x->as.builtin.args[i], k, role, depth)) return false;
            }
            return true;
        }
        case EX_IF:
            return serial_k_use_ok(x->as.if_.cond, k, role, depth) &&
                   serial_k_use_ok(x->as.if_.then_, k, role, depth) &&
                   serial_k_use_ok(x->as.if_.else_or_null, k, role, depth);
        case EX_DO:
            for (uint32_t i = 0; i < x->as.do_.n; i++)
                if (!serial_k_use_ok(x->as.do_.items[i], k, role, depth)) return false;
            return true;
        case EX_LET:
            for (uint32_t i = 0; i < x->as.let_.n; i++) {
                const Expr *init = x->as.let_.bindings[i].init;
                const Binding *lb = x->as.let_.bindings[i].binding;
                /* `(let [g (fn [x] (k x))] ...)` -- the elaborator hoists a
                 * lambda argument this way.  Then g is what must not escape. */
                if (lb && serial_k_closure_body_ok(init, k, role, depth)) {
                    for (uint32_t j = i + 1; j < x->as.let_.n; j++)
                        if (!serial_k_use_ok(x->as.let_.bindings[j].init, lb,
                                             K_FNPARAM, depth))
                            return false;
                    if (!serial_k_use_ok(x->as.let_.body, lb, K_FNPARAM, depth))
                        return false;
                    continue;
                }
                if (!serial_k_use_ok(init, k, role, depth)) return false;
            }
            return serial_k_use_ok(x->as.let_.body, k, role, depth);
        case EX_LETREC:
            for (uint32_t i = 0; i < x->as.let_.n; i++)
                if (!serial_k_use_ok(x->as.let_.bindings[i].init, k, role, depth))
                    return false;
            return serial_k_use_ok(x->as.let_.body, k, role, depth);
        case EX_MATCH:
            if (!serial_k_use_ok(x->as.match_.scrutinee, k, role, depth)) return false;
            for (uint32_t i = 0; i < x->as.match_.n_arms; i++)
                if (!serial_k_use_ok(x->as.match_.arms[i].guard, k, role, depth) ||
                    !serial_k_use_ok(x->as.match_.arms[i].body, k, role, depth))
                    return false;
            return true;
        default:
            return !serial_k_mentions(x, k);
    }
}

static bool serial_recv_confines_k(const CTerm *t) {
    const FnDef *fd = NULL;
    if (t->as.cloneable.receiver_expr) {
        const Expr *f = t->as.cloneable.receiver_expr;
        if (f->kind == EX_CLOSURE && f->as.closure_.closure)
            fd = f->as.closure_.closure->fn;
    } else if (t->as.cloneable.receiver && g_prog) {
        fd = fd_for_binding(g_prog, t->as.cloneable.receiver);
    }
    if (!fd || !fd->body || fd->n_params < 1) return false;
    if (fd->body->kind == EX_INLINE_C || (fd->binding && fd->binding->body_is_inline_c))
        return false;
    const Binding *k = fd->params[fd->n_params - 1];
    return k && serial_k_use_ok(fd->body, k, K_SERIAL, 0);
}

static void emit_cl_shift_bodyfn(CE *ce, const char *bodyfn, const CTerm *t,
                                 const char *cont_setup, const char *cont_arg,
                                 bool free_cap) {
    if (t->as.cloneable.receiver_expr) {
        const Expr *f = t->as.cloneable.receiver_expr;
        struct Closure *closure = f->as.closure_.closure;
        char *thunk_name;
        if (closure->fn->binding) {
            thunk_name = raw_name_for_binding(closure->fn->binding);
        } else {
            thunk_name = malloc(64);
            snprintf(thunk_name, 64, "__fn_anon_%d",
                     closure->fn->n_params > 0 ? closure->fn->params[0]->id : 0);
        }
        /* The thunk's first param is the closure env (void*); its second is the
         * receiver's continuation param `k`.  Cast each arg to the thunk's actual
         * param type so the generated call is warning-clean.  The k param's C type
         * is set by the family, not decidable from the param kind (a cloneable
         * `:cont` receiver and a serial `ptr<void>` receiver both carry kind
         * TY_PTR_VOID): the cloneable receiver is handed a `tur_cloneable_cont *`
         * that rides the int64_t carrier, so its thunk param is int64_t, while the
         * serial receiver is handed a raw DK chain typed `ptr<void>`, so its thunk
         * param is void*.  A blanket int64_t cast makes a pointer from an integer
         * for the serial void* k; a blanket void* cast makes an integer from a
         * pointer for the cloneable int64_t k.  A serial k is itself either
         * spelling (serial_recv_kty). */
        const char *kty = t->as.cloneable.serial ? serial_recv_kty(ce, t) : "int64_t";
        if (free_cap)
            /* serial-cont-chain-never-freed: the receiver kept nothing of
             * `k` (serial_recv_confines_k), so its chain dies here. */
            buf_printf(ce->helpers,
                "static intptr_t %s(intptr_t env, DK *subk) {\n%s"
                "    intptr_t __r = (intptr_t)%s((void *)env, (%s)(intptr_t)%s);\n"
                "    if (!tur_async_suspended) dk_free(%s);\n"
                "    return __r;\n}\n",
                bodyfn, cont_setup, thunk_name, kty, cont_arg, cont_arg);
        else
            buf_printf(ce->helpers,
                "static intptr_t %s(intptr_t env, DK *subk) {\n%s"
                "    return (intptr_t)%s((void *)env, (%s)(intptr_t)%s);\n}\n",
                bodyfn, cont_setup, thunk_name, kty, cont_arg);
        free(thunk_name);
    } else {
        /* Named-fn receiver: cast the receiver fn ptr (threaded through `env`)
         * to its ACTUAL param signature, not a blanket `int64_t (*)(int64_t)`.
         * A serial receiver's continuation param `k` is `ptr<void>` -- its real C
         * signature is `int64_t rt(void *)` -- so a uniform int64_t cast invokes
         * it through an incompatible function-pointer type.  That is UB a
         * `-fsanitize=function` / CFI build trips on, even though it works on every
         * supported ABI (`void *` and `int64_t` are passed identically in an
         * integer register).  Key the callee pointer type AND its argument cast on
         * the same `serial` bit the closure branch uses two cases up for the
         * continuation-arg cast; the non-serial (cloneable) case keeps int64_t, so
         * its emitted C is byte-identical to before.  (A serial k's own
         * spelling is read off the receiver's declared parameter.) */
        const char *kty = t->as.cloneable.serial ? serial_recv_kty(ce, t) : "int64_t";
        /* fnsan-cont-receiver-result: and its RESULT at the receiver's recorded
         * return type -- `const char *cl(int64_t)` for a `(cont cstr)`
         * receiver was called as returning `int64_t`.  The value leaves as the
         * word the trampoline carries (a float as its bits). */
        const char *rty = NULL;
        if (t->as.cloneable.receiver) {
            char *rn = raw_name_for_binding(t->as.cloneable.receiver);
            rty = rn ? emit_sig_lookup_ret_ctype(rn) : NULL;
            free(rn);
        }
        if (rty && *rty) {
            size_t RL = strlen(rty);
            bool scalar = rty[RL - 1] == '*' || strcmp(rty, "double") == 0 ||
                          strcmp(rty, "float") == 0 || strcmp(rty, "bool") == 0 ||
                          strcmp(rty, "int8_t") == 0 || strcmp(rty, "int16_t") == 0 ||
                          strcmp(rty, "int32_t") == 0 || strcmp(rty, "uint8_t") == 0 ||
                          strcmp(rty, "uint16_t") == 0 || strcmp(rty, "uint32_t") == 0 ||
                          strcmp(rty, "uint64_t") == 0;
            if (!scalar) rty = NULL;   /* an aggregate keeps today's word call */
        }
        if (!rty) rty = "int64_t";
        Buf rc; buf_init(&rc);
        Buf call; buf_init(&call);
        buf_printf(&call, "((%s (*)(%s))(intptr_t)env)((%s)(intptr_t)%s)",
                   rty, kty, kty, cont_arg);
        buf_putc(&call, '\0');
        emit_scalar_word_conv(&rc, rty, "int64_t", call.data);
        buf_putc(&rc, '\0');
        if (free_cap)
            buf_printf(ce->helpers,
                "static intptr_t %s(intptr_t env, DK *subk) {\n%s"
                "    intptr_t __r = (intptr_t)%s;\n"
                "    if (!tur_async_suspended) dk_free(%s);\n"
                "    return __r;\n}\n",
                bodyfn, cont_setup, rc.data, cont_arg);
        else
            buf_printf(ce->helpers,
                "static intptr_t %s(intptr_t env, DK *subk) {\n%s"
                "    return (intptr_t)%s;\n}\n",
                bodyfn, cont_setup, rc.data);
        buf_free(&rc);
        buf_free(&call);
    }
}

/* The dk_shift env expression (malloc'd) for a Shape 2 node: a closure receiver's
 * env value -- emitted at the reset site so its captures are read from the visible
 * locals / lifted env -- else the named receiver's fn name. */
static char *emit_cl_shift_env(CE *ce, const CTerm *t, const char *rfn) {
    if (t->as.cloneable.receiver_expr) {
        int saved = ce->ctx->indent;
        ce->ctx->indent = ce->indent;
        char *fval = emit_value(ce->ctx, ce->out, t->as.cloneable.receiver_expr);
        ce->ctx->indent = saved;
        return fval;
    }
    return strdup(rfn);
}

/* The closure receiver's fat box (emit_cl_shift_env) rides only the dk_shift
 * node's env, which the shift body reads to call the receiver; the chain it
 * captures starts BELOW the shift node, so no resume copy ever sees it.  Once
 * the run has returned it is dead -- release it, as a consumed closure literal
 * is released everywhere else (catch-unwind's thunk).  A named receiver's env
 * is its code address.  See docs/archive/serial-cont-chain-never-freed.md. */
static void emit_cl_shift_env_drop(CE *ce, const CTerm *t, const char *senv) {
    const Expr *f = t->as.cloneable.receiver_expr;
    if (f && f->kind == EX_CLOSURE)
        ce_line(ce, "if (!tur_async_suspended) TUR_CLOSURE_DROP(%s);", senv);
}

/* serial-receiver-effect-cannot-reach-enclosing-handler: the tail of an
 * outward serial reset -- `return <receiver>__cps(<k>, <rest frame>)`.  The
 * rest (the cloneable node's body, binding x) is lifted as a resume-frame over
 * `cur_k` exactly as emit_heap_join lifts a join, so the receiver's result
 * binds x and the enclosing continuation -- with every handler installed above
 * this function -- stays reachable from the receiver's own chain. */
static void emit_serial_outward_call(CE *ce, const CTerm *t, int id, const char *kchain) {
    char jname[256];
    snprintf(jname, sizeof(jname), "%s_skj%d", ce->fn_cn, id);
    char *xn = cvar_cname(ce, t->as.cloneable.x);
    CapSet cs;
    bool caps_ok = collect_caps(t->as.cloneable.body, t->as.cloneable.x.id, &cs);
    const CapSet *caps = (caps_ok && cs.n > 0) ? &cs : NULL;
    emit_lifted(ce, jname, LH_RESUME_CONT, xn, t->as.cloneable.x.ty,
                t->as.cloneable.x.type, t->as.cloneable.body, NULL, caps);
    free(xn);
    /* serial-cont-chain-never-freed: the receiver owns `kchain`; when it
     * keeps nothing of it (serial_recv_confines_k) the chain is dead once the
     * receiver is done, which is inside this entry's dynamic extent -- so it is
     * registered for the entry boundary's reap.  It cannot be freed after the
     * call: the call is this function's tail. */
    char kreap[96];
    if (serial_recv_confines_k(t)) {
        snprintf(kreap, sizeof kreap, "__dk_reap_keep(%s)", kchain);
        kchain = kreap;
    }
    char *envexpr = emit_cont_env(ce, jname, caps);
    const char *kty = serial_recv_kty(ce, t);
    if (t->as.cloneable.receiver_expr) {
        /* serial-receiver-effect-under-if-closure-or-leaf (shape 2): a closure
         * receiver.  Its value is emitted here, at the reset site, so its
         * captures are read from the visible locals (or the lifted env), and
         * its lifted lambda's `__cps` twin takes that value as the env it
         * reads them through -- the same first argument the direct thunk
         * gets on the native path (emit_cl_shift_bodyfn). */
        const Expr *f = t->as.cloneable.receiver_expr;
        int saved = ce->ctx->indent;
        ce->ctx->indent = ce->indent;
        char *fval = emit_value(ce->ctx, ce->out, f);
        ce->ctx->indent = saved;
        const FnDef *lfd = f->as.closure_.closure->fn;
        char *thunk = raw_name_for_binding(lfd->binding);
        const char *envty = lfd->closure && lfd->n_params > 1
                          ? emit_param_ctype(ce->ctx, (FnDef *)lfd, 0) : NULL;
        if (envty)
            ce_line(ce, "return %s__cps((%s)(intptr_t)(%s), (%s)(intptr_t)%s,"
                        " __dk_reap_node(dk_frame_resume(%s, %s, %s)));"
                        " /* serial reset: outward closure receiver */",
                    thunk, envty, fval, kty, kchain, jname, envexpr, ce->cur_k);
        else
            ce_line(ce, "return %s__cps((%s)(intptr_t)%s,"
                        " __dk_reap_node(dk_frame_resume(%s, %s, %s)));"
                        " /* serial reset: outward closure receiver */",
                    thunk, kty, kchain, jname, envexpr, ce->cur_k);
        free(thunk);
        free(fval);
    } else {
        char *fn = callee_name(t->as.cloneable.receiver);
        ce_line(ce, "return %s__cps((%s)(intptr_t)%s, __dk_reap_node(dk_frame_resume(%s, %s, %s)));"
                    " /* serial reset: outward receiver */",
                fn, kty, kchain, jname, envexpr, ce->cur_k);
        free(fn);
    }
    free(envexpr);
    /* serial-receiver-effect-under-if-closure-or-leaf (shape 1): with an `if`
     * branch point in the context, the shift arm above ends in the outward
     * call, and the PURE arm delivers its value -- the outer frames re-applied,
     * exactly as the native lowering yields it -- into the same lifted rest.
     * Both arms then share one continuation: the rest is emitted once, and an
     * effect performed further down it still reaches the handlers above. */
    if (t->as.cloneable.if_cond) {
        ce->indent--;
        ce_line(ce, "} else {");
        ce->indent++;
        char *pv = emit_cloneable_pure_arm(ce, t);
        char *sv = slot_store_reap(ce->ctx, t->as.cloneable.x.ty,
                                   t->as.cloneable.x.type, pv);
        char *env2 = emit_cont_env(ce, jname, caps);
        ce_line(ce, "return dk_run(__dk_reap_node(dk_frame_resume(%s, %s, %s)), %s);"
                    " /* serial reset: pure arm into the outward rest */",
                jname, env2, ce->cur_k, sv);
        free(env2);
        free(sv);
        free(pv);
        ce->indent--;
        ce_line(ce, "}");
    }
}

static void emit_cloneable(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char *xn  = cvar_cname(ce, t->as.cloneable.x);
    /* Named-fn receiver: its C name.  A closure receiver (receiver_expr, Shape 1)
     * has no bare fn name -- it is emitted as a value + thunk call below. */
    char *rfn = t->as.cloneable.receiver ? callee_name(t->as.cloneable.receiver) : NULL;

    /* Pure `let` prelude (Shape 2, let-bearing): lay each binding down as a C
     * local at the reset site, ahead of the frame-operand evaluations that may
     * reference it.  Emitted before any `if` branch point below, so a `let` above
     * an `if` is in scope for both the outer frame operands (shift arm) and the
     * pure arm. */
    for (uint32_t li = 0; li < t->as.cloneable.n_lets; li++) {
        const CloneLet *cl = &t->as.cloneable.lets[li];
        char *iv = emit_cloneable_direct(ce, cl->init);
        if (!cl->binding) {
            /* Side-effect prelude (do-sequence prefix): emit for effect only. */
            ce_line(ce, "(void)(%s);", iv);
            free(iv);
            continue;
        }
        char *bn = name_for_binding(ce->ctx, cl->binding);
        ce_line(ce, "%s %s = %s;", emit_type_c_name(ce->ctx, cl->binding->type), bn, iv);
        ce_line(ce, "(void)%s;", bn);
        free(bn);
        free(iv);
    }

    /* One `if` branch point (Shape 2, if-bearing): evaluate the pure condition
     * once, run the DK chain on the shift-bearing arm, and yield the direct-
     * emitted pure arm on the other branch. */
    bool has_if = (t->as.cloneable.if_cond != NULL);
    if (has_if) {
        char *cv = emit_cloneable_direct(ce, t->as.cloneable.if_cond);
        ce_line(ce, "if (%s(%s)) {", t->as.cloneable.if_when ? "" : "!", cv);
        free(cv);
        ce->indent++;
    }

    if (t->as.cloneable.n_frames == 0 && !t->as.cloneable.serial) {
        /* Shape 1 (cloneable): identity continuation.  The serial Shape 1
         * (n_frames == 0 && serial) falls through to the serial branch below,
         * where the empty frame loops produce a bare marshalable prompt chain --
         * a cloneable_cont would not round-trip through save-cont!/resume-cont!. */
        char cfn[256];
        snprintf(cfn, sizeof(cfn), "%s_clc%d", ce->fn_cn, id);
        buf_printf(ce->helpers,
            "static int64_t %s(void *__e, int64_t __v) { (void)__e; return __v; }\n", cfn);
        char cv[64];
        snprintf(cv, sizeof(cv), "__clc%d", id);
        ce_line(ce, "tur_cloneable_cont *%s = tur_cloneable_cont_alloc(%s, NULL, NULL, NULL);",
                cv, cfn);
        if (t->as.cloneable.receiver_expr) {
            /* U7 closure receiver: emit the closure value (its captures ride the
             * env -- visible locals in the main body, or env-carried in a lifted
             * body per collect_caps), then call its thunk with (closure-env, cont),
             * mirroring emit_callcc's closure call. */
            const Expr *f = t->as.cloneable.receiver_expr;
            int saved = ce->ctx->indent;
            ce->ctx->indent = ce->indent;
            char *fval = emit_value(ce->ctx, ce->out, f);
            ce->ctx->indent = saved;
            struct Closure *closure = f->as.closure_.closure;
            char *thunk_name;
            if (closure->fn->binding) {
                thunk_name = raw_name_for_binding(closure->fn->binding);
            } else {
                thunk_name = malloc(64);
                snprintf(thunk_name, 64, "__fn_anon_%d",
                         closure->fn->n_params > 0 ? closure->fn->params[0]->id : 0);
            }
            ce_line(ce, "%s = %s(%s, (int64_t)(intptr_t)%s);", xn, thunk_name, fval, cv);
            free(thunk_name);
            free(fval);
        } else {
            ce_line(ce, "%s = %s((int64_t)(intptr_t)%s);", xn, rfn, cv);
        }
    } else if (t->as.cloneable.serial) {
        /* U4 Shape 2 (serial): an arithmetic context marshaled via the shared
         * tagged frames (`__sk_frame_for_tag`), and a shift body that hands the
         * receiver the copied DK chain directly so the captured continuation
         * round-trips through save-cont!/resume-cont!.  No per-site frame fns. */
        uint32_t nf = t->as.cloneable.n_frames;
        bool outward = t->as.cloneable.recv_outward;
        char bodyfn[256];
        snprintf(bodyfn, sizeof(bodyfn), "%s_skbody%d", ce->fn_cn, id);
        if (!outward)
            emit_cl_shift_bodyfn(ce, bodyfn, t,
                "    DK *__cap = dk_copy_range(subk, NULL);\n", "__cap",
                serial_recv_confines_k(t));
        /* A 1-arg call frame gets a per-site wrapper fn plus a SkReg entry that
         * self-registers (constructor) so the marshaler maps the frame <-> a stable
         * name ("<fn>$L") for save/restore.  Arithmetic frames need no per-site
         * emission -- they ride the shared tagged marshaler. */
        for (uint32_t i = 0; i < nf; i++) {
            const CloneFrame *fr = &t->as.cloneable.frames[i];
            if (!fr->call_fn) continue;
            char *cfn = callee_name(fr->call_fn);
            /* A do-tail ignore-value frame runs f() regardless of the resumed
             * value and marshals under the "$0" side; a 1-arg hole call applies f
             * to the resumed value under "$L"; a 2-arg call applies f to the
             * resumed value + the captured int env (source order per hole side)
             * under "$2L"/"$2R", the env serialized inline. */
            uint32_t ar = fr->call_fn->type.as.fn.arity;
            bool two_arg = !fr->ignore_value && ar == 2;
            bool env1    = fr->ignore_value && ar == 1;  /* 1-arg captured-config tail */
            bool has_env = two_arg || env1;
            /* Env marshal kind: SK_ENV_INT (0) / SK_ENV_CSTR (1) inline codec, or
             * SK_ENV_SER (2) via the env type's Serializable instance.  Keyed off
             * the captured operand's type. */
            int ekc = 0;
            char *eser = NULL, *edeser = NULL;
            memset(&g_serial_env_sigs, 0, sizeof g_serial_env_sigs);
            if (has_env) {
                if (fr->operand.ty == TY_CSTR) ekc = 1;
                else if (fr->operand.type
                         && serial_env_ser_names(ce, fr->operand.type, &eser, &edeser))
                    ekc = 2;
            }
            const char *ecast = ekc == 1 ? "(const char *)" : "(int64_t)";
            const char *side;
            if (fr->ignore_value && ar == 0) {
                buf_printf(ce->helpers,
                    "static intptr_t %s_skcall%d_%u(intptr_t env, intptr_t value) { (void)env; (void)value; return (intptr_t)%s(); }\n",
                    ce->fn_cn, id, i, cfn);
                side = "$0";
            } else if (env1) {
                /* Run f(cap) on resume, ignoring the resumed value; cap = the env.
                 * opaque-pointer-c-spelling: the env cast used to be `ecast`
                 * alone -- the marshaller's codec kind -- while the two- and
                 * one-hole branches beside it ask `cc_cast_for_param`, which
                 * knows the difference between "this is how the env was
                 * marshalled" and "this is what the callee's slot is spelled".
                 * They only diverge once a param can be a pointer that the
                 * default carrier cast straddles: a `(defopaque Rec :ptr<void>)`
                 * env deserialized through its Serializable instance reached
                 * `run_loop(void *)` as `(int64_t)env`.  Keep the explicit cstr
                 * codec cast, which is already the right pointer. */
                char esc[192];
                cc_cast_for_param(ce, fr->call_fn, 0, esc, sizeof esc);
                buf_printf(ce->helpers,
                    "static intptr_t %s_skcall%d_%u(intptr_t env, intptr_t value) { (void)value; return (intptr_t)%s(%senv); }\n",
                    ce->fn_cn, id, i, cfn, ekc == 1 ? ecast : esc);
                side = "$E";
            } else if (two_arg) {
                const char *a0 = fr->hole_left ? "value" : "env";
                const char *a1 = fr->hole_left ? "env" : "value";
                char sc0[192], sc1[192];
                cc_cast_for_param(ce, fr->call_fn, 0, sc0, sizeof sc0);
                cc_cast_for_param(ce, fr->call_fn, 1, sc1, sizeof sc1);
                buf_printf(ce->helpers,
                    "static intptr_t %s_skcall%d_%u(intptr_t env, intptr_t value) { return (intptr_t)%s(%s%s, %s%s); }\n",
                    ce->fn_cn, id, i, cfn,
                    sc0, a0, sc1, a1);
                side = fr->hole_left ? "$2L" : "$2R";
            } else {
                char sc0[192];
                cc_cast_for_param(ce, fr->call_fn, 0, sc0, sizeof sc0);
                buf_printf(ce->helpers,
                    "static intptr_t %s_skcall%d_%u(intptr_t env, intptr_t value) { (void)env; return (intptr_t)%s(%svalue); }\n",
                    ce->fn_cn, id, i, cfn, sc0);
                side = "$L";
            }
            if (ekc == 2) {
                /* SER env: carry the instance serialize/deserialize fns, each
                 * behind an adapter at the registry's slot type when its own
                 * signature differs (fnsan-serial-registry-hooks). */
                const SerSigs *sg = &g_serial_env_sigs;
                char sers[160], desers[160];
                snprintf(sers, sizeof sers, "%s", eser);
                snprintf(desers, sizeof desers, "%s", edeser);
                if (sg->ser_p && sg->ser_r &&
                    (strcmp(sg->ser_p, "int64_t") != 0 || strcmp(sg->ser_r, "void *") != 0)) {
                    snprintf(sers, sizeof sers, "%s_sks%d_%u", ce->fn_cn, id, i);
                    buf_printf(ce->helpers, "static void *%s(int64_t e) {\n    %s r = %s(",
                               sers, sg->ser_r, eser);
                    emit_scalar_word_conv(ce->helpers, "int64_t", sg->ser_p, "e");
                    buf_puts(ce->helpers, ");\n    return ");
                    emit_scalar_word_conv(ce->helpers, sg->ser_r, "void *", "r");
                    buf_puts(ce->helpers, ";\n}\n");
                }
                if (sg->deser_p && sg->deser_r &&
                    (strcmp(sg->deser_p, "void *") != 0 || strcmp(sg->deser_r, "int64_t") != 0)) {
                    snprintf(desers, sizeof desers, "%s_skd%d_%u", ce->fn_cn, id, i);
                    buf_printf(ce->helpers, "static int64_t %s(void *b) {\n    %s r = %s(",
                               desers, sg->deser_r, edeser);
                    emit_scalar_word_conv(ce->helpers, "void *", sg->deser_p, "b");
                    buf_puts(ce->helpers, ");\n    return ");
                    emit_scalar_word_conv(ce->helpers, sg->deser_r, "int64_t", "r");
                    buf_puts(ce->helpers, ";\n}\n");
                }
                buf_printf(ce->helpers,
                    "static SkReg %s_skreg%d_%u = { \"%s%s\", %s_skcall%d_%u, %d,"
                    " (void *(*)(int64_t))%s, (int64_t (*)(void *))%s, 0 };\n"
                    "static void %s_skreginit%d_%u(void) { __sk_register(&%s_skreg%d_%u); }\n",
                    ce->fn_cn, id, i, cfn, side, ce->fn_cn, id, i, ekc, sers, desers,
                    ce->fn_cn, id, i, ce->fn_cn, id, i);
            } else {
                buf_printf(ce->helpers,
                    "static SkReg %s_skreg%d_%u = { \"%s%s\", %s_skcall%d_%u, %d, 0, 0, 0 };\n"
                    "static void %s_skreginit%d_%u(void) { __sk_register(&%s_skreg%d_%u); }\n",
                    ce->fn_cn, id, i, cfn, side, ce->fn_cn, id, i, ekc,
                    ce->fn_cn, id, i, ce->fn_cn, id, i);
            }
            /* S1b: the session-kind registry must be populated before any
             * effectful call dispatches through it. */
            char skinit[320];
            snprintf(skinit, sizeof(skinit), "%s_skreginit%d_%u", ce->fn_cn, id, i);
            static_init_register(skinit, STATIC_INIT_REGISTRY);
            free(eser); free(edeser);
            free(cfn);
        }
        char dv[48];
        snprintf(dv, sizeof(dv), "__skd%d", id);
        ce_line(ce, "DK *%s = dk_prompt(1, dk_done());", dv);
        /* Push frames outermost-first (frames[0] deepest, closest to the prompt).
         * Arithmetic -> shared tagged frame + operand; call frame -> per-site
         * wrapper, no env (0). */
        for (uint32_t i = 0; i < nf; i++) {
            const CloneFrame *fr = &t->as.cloneable.frames[i];
            if (fr->call_fn) {
                /* A 2-arg hole call or a 1-arg captured-config do-tail frame carries
                 * the captured env; a 1-arg hole call / 0-arg do-tail pass 0.  The
                 * env rides q->env and is marshaled by kind (int/cstr inline, or a
                 * Serializable instance).  A non-atomic env is emit_value'd here at
                 * the reset site (once, cold-start capture). */
                uint32_t ar = fr->call_fn->type.as.fn.arity;
                bool has_env = (!fr->ignore_value && ar == 2)
                            || (fr->ignore_value && ar == 1);
                char *opv = NULL;
                if (has_env)
                    opv = fr->env_expr ? emit_cloneable_direct(ce, fr->env_expr)
                                       : atom_str(ce, &fr->operand);
                ce_line(ce, "%s = dk_frame(%s_skcall%d_%u, (intptr_t)(%s), %s);",
                        dv, ce->fn_cn, id, i, opv ? opv : "0", dv);
                free(opv);
            } else {
                /* D6a parity: a non-atomic pure arithmetic operand (env_expr) is
                 * emit_value'd at the reset site; its int value rides the tagged
                 * frame env, serialized inline by the shared marshaler. */
                char *opv = fr->env_expr ? emit_cloneable_direct(ce, fr->env_expr)
                                         : atom_str(ce, &fr->operand);
                ce_line(ce, "%s = dk_frame(__sk_frame_for_tag(%d), (intptr_t)(%s), %s);",
                        dv, sk_tag_for_frame(fr), opv, dv);
                free(opv);
            }
        }
        if (outward) {
            /* serial-receiver-effect-cannot-reach-enclosing-handler: `dv` is
             * now exactly what the shift would have captured -- the context
             * frames over a fresh prompt -- so hand it to the receiver
             * directly, as an ordinary colored call whose continuation is the
             * rest of this function (lifted, like a heap join).  An effect the
             * receiver performs walks that continuation out into the handlers
             * enclosing the reset.  The receiver owns `dv`, as it owned the
             * shift body's copy. */
            emit_serial_outward_call(ce, t, id, dv);
            free(xn);
            free(rfn);
            return;
        }
        char *senv = emit_cl_shift_env(ce, t, rfn);
        ce_line(ce, "%s = dk_shift(1, %s, (intptr_t)(%s), %s);", dv, bodyfn, senv, dv);
        ce_line(ce, "%s = (%s)dk_run(%s, 0);", xn,
                binder_ctype_full(ce->ctx, t->as.cloneable.x.ty, t->as.cloneable.x.type), dv);
        ce_line(ce, "dk_free(%s);", dv);
        emit_cl_shift_env_drop(ce, t, senv);
        free(senv);
    } else {
        /* Shape 2: an arithmetic context (1+ frames) + dk_copy_range capture. */
        uint32_t nf = t->as.cloneable.n_frames;
        char bodyfn[256];
        snprintf(bodyfn, sizeof(bodyfn), "%s_ccbody%d", ce->fn_cn, id);
        /* One frame fn per context frame: an arithmetic op, a 1-arg call to a
         * top-level fn applied to the resumed value (no captured env), or a 2-arg
         * call whose non-hole arg rides the frame env (deep-cloned with the chain
         * on each resume, so multi-shot is correct without any marshaling). */
        for (uint32_t i = 0; i < nf; i++) {
            char ctxfn[256];
            snprintf(ctxfn, sizeof(ctxfn), "%s_ccctx%d_%u", ce->fn_cn, id, i);
            const CloneFrame *fr = &t->as.cloneable.frames[i];
            if (fr->call_fn) {
                char *cfn = callee_name(fr->call_fn);
                bool two_arg = !fr->ignore_value && fr->call_fn->type.as.fn.arity == 2;
                if (fr->ignore_value)
                    /* 0-arg ignore-value tail: run f() regardless of resume value. */
                    buf_printf(ce->helpers,
                        "static intptr_t %s(intptr_t env, intptr_t value) { (void)env; (void)value; return (intptr_t)%s(); }\n",
                        ctxfn, cfn);
                else if (two_arg) {
                    /* 2-arg call: apply f to (value, env) in source order (env =
                     * the captured non-hole arg), casting each to its param kind. */
                    const char *a0 = fr->hole_left ? "value" : "env";
                    const char *a1 = fr->hole_left ? "env" : "value";
                    /* E3a widening: an owning by-value AGGREGATE env rides the
                     * one-word slot by ADDRESS (the push emits `&o`).  Its arg cast
                     * is not a scalar cast: deref the pointer for a by-value param
                     * (<=16B), or pass the pointer for a pass-by-ptr param (>16B).
                     * The hole arg stays a scalar cast (the resumed value).  The
                     * scalar side uses cc_cast_for_param so a concrete-pointer param
                     * (`tur_adt_H *`) is bridged, not straddled by an int64 cast. */
                    char c0[192], c1[192];
                    if (fr->operand.type
                        && owning_byvalue_aggregate(fr->operand.type)) {
                        const char *cn = emit_type_c_name(ce->ctx, *fr->operand.type);
                        bool pbp = type_struct_pass_by_ptr(*fr->operand.type);
                        /* env arg is a0 when the hole is on the right, else a1.
                         * Both casts spelled as literals so -Wformat checks them. */
                        char *envc = fr->hole_left ? c1 : c0;
                        size_t envn = fr->hole_left ? sizeof c1 : sizeof c0;
                        if (pbp) snprintf(envc, envn, "(const %s *)", cn);
                        else     snprintf(envc, envn, "*(%s *)", cn);
                        if (fr->hole_left) {
                            cc_cast_for_param(ce, fr->call_fn, 0, c0, sizeof c0);
                        } else {
                            cc_cast_for_param(ce, fr->call_fn, 1, c1, sizeof c1);
                        }
                    } else {
                        cc_cast_for_param(ce, fr->call_fn, 0, c0, sizeof c0);
                        cc_cast_for_param(ce, fr->call_fn, 1, c1, sizeof c1);
                    }
                    buf_printf(ce->helpers,
                        "static intptr_t %s(intptr_t env, intptr_t value) { return (intptr_t)%s(%s%s, %s%s); }\n",
                        ctxfn, cfn, c0, a0, c1, a1);
                    /* E3a: a CONSUMING rc frame drops its rc arg once per call, so
                     * a multi-shot resume needs its own +1.  Emit the env_clone glue
                     * (rc incref) that dk_copy_node runs on each resume copy; the
                     * frame's own drop balances it (no env_drop -- the base +1 is
                     * released once by the owner's scope-exit drop). */
                    if (cloneable_frame_consumes_rc(fr))
                        buf_printf(ce->helpers,
                            "static intptr_t %s_envclone(intptr_t e) { "
                            "rc_strong_increment((RcControlBlock *)(intptr_t)e); return e; }\n",
                            ctxfn);
                    else if (cloneable_frame_consumes_carrier(fr)) {
                        /* A heap carrier: deep-copy the pointed-to header (malloc a
                         * fresh header + shallow field copy) so each resume frees
                         * its OWN allocation, then INCREF each owning (rc/weak)
                         * field so the copy owns its own +1 (its drop_glue decref
                         * on free balances it) -- the shallow copy shares the owning
                         * field's control block with the original, exactly as the
                         * owner's own drop expects.  A flat handle (no owning fields)
                         * gets an empty incref list.  The handle's C type is
                         * `<hdr> *`; strip the trailing ` *` for the header name. */
                        char hdr[192];
                        snprintf(hdr, sizeof hdr, "%s",
                                 emit_type_c_name(ce->ctx, *fr->operand.type));
                        size_t L = strlen(hdr);
                        while (L > 0 && (hdr[L-1] == '*' || hdr[L-1] == ' '))
                            hdr[--L] = 0;
                        Buf incs; buf_init(&incs);
                        const AdtDef *d = cloneable_carrier_def(fr->operand.type);
                        const CtorDef *c = d ? d->ctors[0] : NULL;
                        for (uint32_t fi = 0; c && fi < c->n_fields; fi++) {
                            TypeKind fk = c->fields[fi].kind;
                            char *mp = adt_field_member_path(d, c, fi);
                            if (fk == TY_RC || fk == TY_WEAK) {
                                buf_printf(&incs, "if (__c->%s) %s(__c->%s); ",
                                           mp, fk == TY_RC ? "rc_strong_increment"
                                                           : "rc_weak_increment", mp);
                            } else if (fk == TY_REF || fk == TY_LREF) {
                                /* deep-copy the ref<scalar> box: a fresh malloc +
                                 * scalar copy, so each resume frees its own box
                                 * (mirrors drop_glue's `free(s->f)`). */
                                Type it; memset(&it, 0, sizeof it);
                                it.kind = c->fields[fi].inner_kind;
                                const char *innerc = type_c_name(it);
                                /* the field is stored `void *`; cast to the inner
                                 * pointer type to deref, and store the fresh box
                                 * back as `void *`. */
                                buf_printf(&incs,
                                    "if (__c->%s) { %s *__rb = (%s *)malloc(sizeof(%s)); "
                                    "*__rb = *(%s *)(__c->%s); __c->%s = (void *)__rb; } ",
                                    mp, innerc, innerc, innerc, innerc, mp, mp);
                            }
                            free(mp);
                        }
                        buf_putc(&incs, '\0');
                        buf_printf(ce->helpers,
                            "static intptr_t %s_envclone(intptr_t e) { "
                            "%s *__c = (%s *)malloc(sizeof(%s)); "
                            "*__c = *(%s *)(intptr_t)e; %sreturn (intptr_t)__c; }\n",
                            ctxfn, hdr, hdr, hdr, hdr, incs.data ? incs.data : "");
                        buf_free(&incs);
                    }
                    else if (cloneable_frame_consumes_aggregate(fr)) {
                        /* The aggregate rides the env by ADDRESS (`&o`) and is
                         * passed BY VALUE to the callee, which drops its own copy.
                         * Incref each owning (rc/weak) field via `&o` so the copy's
                         * drop is balanced -- return the SAME `&o` (no allocation;
                         * `o` is the owner's stable local, freed once by its own
                         * scope-exit drop). */
                        const char *cn = emit_type_c_name(ce->ctx, *fr->operand.type);
                        Buf incs; buf_init(&incs);
                        const Type *t = fr->operand.type;
                        const AdtDef *d = (t->kind == TY_ADT) ? t->as.adt_.def
                                        : (t->kind == TY_APP) ? type_adt_app_def((Type *)t)
                                        : NULL;
                        const CtorDef *c = d ? d->ctors[0] : NULL;
                        for (uint32_t fi = 0; c && fi < c->n_fields; fi++) {
                            TypeKind fk = c->fields[fi].kind;
                            if (fk != TY_RC && fk != TY_WEAK) continue;
                            char *mp = adt_field_member_path(d, c, fi);
                            buf_printf(&incs,
                                "if (((%s *)(intptr_t)e)->%s) %s(((%s *)(intptr_t)e)->%s); ",
                                cn, mp, fk == TY_RC ? "rc_strong_increment"
                                                    : "rc_weak_increment", cn, mp);
                            free(mp);
                        }
                        buf_putc(&incs, '\0');
                        buf_printf(ce->helpers,
                            "static intptr_t %s_envclone(intptr_t e) { %sreturn e; }\n",
                            ctxfn, incs.data ? incs.data : "");
                        buf_free(&incs);
                    }
                } else {
                    /* 1-arg hole call: apply f to the resumed value. */
                    char sc0[192];
                    cc_cast_for_param(ce, fr->call_fn, 0, sc0, sizeof sc0);
                    buf_printf(ce->helpers,
                        "static intptr_t %s(intptr_t env, intptr_t value) { (void)env; return (intptr_t)%s(%svalue); }\n",
                        ctxfn, cfn, sc0);
                }
                free(cfn);
            } else {
                buf_printf(ce->helpers,
                    "static intptr_t %s(intptr_t env, intptr_t value) { return %s; }\n",
                    ctxfn, cloneable_frame_expr(fr->op, fr->hole_left));
            }
        }
        emit_cl_shift_bodyfn(ce, bodyfn, t,
            "    DK *__cap = dk_copy_range(subk, NULL);\n"
            "    tur_cloneable_cont *__k = tur_cloneable_cont_alloc(__dk_cont_fn, __cap, __dk_env_clone, __dk_env_drop);\n",
            "__k", false);
        char dv[48];
        snprintf(dv, sizeof(dv), "__ccd%d", id);
        ce_line(ce, "DK *%s = dk_prompt(1, dk_done());", dv);
        /* Push frames outermost-first (frames[0] deepest, closest to the prompt).
         * A 2-arg call frame carries its captured int env; 1-arg / 0-arg call and
         * do-tail frames pass 0. */
        for (uint32_t i = 0; i < nf; i++) {
            char ctxfn[256];
            snprintf(ctxfn, sizeof(ctxfn), "%s_ccctx%d_%u", ce->fn_cn, id, i);
            const CloneFrame *fr = &t->as.cloneable.frames[i];
            bool two_arg = fr->call_fn && !fr->ignore_value
                && fr->call_fn->type.as.fn.arity == 2;
            /* A non-atomic env operand (fr->env_expr set -- an arithmetic frame
             * or a 2-arg call frame) is emit_value'd at the reset site; a 1-arg
             * call frame carries no env (pass 0); everything else rides the atom. */
            char *opv = (fr->call_fn && !two_arg) ? strdup("0")
                      : fr->env_expr ? emit_cloneable_direct(ce, fr->env_expr)
                      : atom_str(ce, &fr->operand);
            /* E3a widening: an owning by-value AGGREGATE operand is captured by
             * ADDRESS -- carry `&(o)` (o is the owner's stable by-value local; the
             * reset runs synchronously in its frame, so the pointer outlives every
             * resume, and a ^borrow frame never drops it).  Only the atomic
             * (bare-lvalue) operand path is admitted for an aggregate, so `opv` is
             * a plain lvalue name here and `&(...)` is well-formed. */
            if (fr->operand.type && owning_byvalue_aggregate(fr->operand.type)
                && !fr->env_expr) {
                char *addr = malloc(strlen(opv) + 8);
                snprintf(addr, strlen(opv) + 8, "&(%s)", opv);
                free(opv); opv = addr;
            }
            /* E3a: a CONSUMING rc frame rides dk_frame_owning with the env_clone
             * glue emitted above (incref per resume copy) and NO env_drop (the
             * frame's own drop balances each copy; the base +1 is the owner's).
             * A borrow / scalar / aggregate frame stays a plain dk_frame. */
            if (cloneable_frame_consumes_rc(fr) || cloneable_frame_consumes_carrier(fr)
                || cloneable_frame_consumes_aggregate(fr))
                ce_line(ce, "%s = dk_frame_owning(%s, (intptr_t)(%s), %s_envclone, 0, %s);",
                        dv, ctxfn, opv, ctxfn, dv);
            else
                ce_line(ce, "%s = dk_frame(%s, (intptr_t)(%s), %s);", dv, ctxfn, opv, dv);
            free(opv);
        }
        char *senv = emit_cl_shift_env(ce, t, rfn);
        ce_line(ce, "%s = dk_shift(1, %s, (intptr_t)(%s), %s);", dv, bodyfn, senv, dv);
        ce_line(ce, "%s = (%s)dk_run(%s, 0);", xn,
                binder_ctype_full(ce->ctx, t->as.cloneable.x.ty, t->as.cloneable.x.type), dv);
        ce_line(ce, "dk_free(%s);", dv);
        emit_cl_shift_env_drop(ce, t, senv);
        free(senv);
    }

    if (has_if) {
        ce->indent--;
        ce_line(ce, "} else {");
        ce->indent++;
        /* The pure arm re-applies the OUTER context frames (those outside the
         * `if`); inner frames belong to the shift-bearing arm only. */
        char *pv = emit_cloneable_pure_arm(ce, t);
        ce_line(ce, "%s = %s;", xn, pv);
        free(pv);
        ce->indent--;
        ce_line(ce, "}");
    }

    free(xn);
    free(rfn);
    emit_term(ce, t->as.cloneable.body);
}

/* CT_SHIFT: capture the sub-continuation up to the nearest enclosing prompt and
 * run the shift body (abortive: the identity receiver delivers the body value,
 * ignoring the captured continuation). */
static void emit_shift(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char hname[256];
    snprintf(hname, sizeof(hname), "%s_s%d", ce->fn_cn, id);
    /* N6.3: the shift body's scalar captures (source vars / CPS vars) ride the
     * shift-body helper's env struct; the captured continuation k is excluded
     * (it is `subk`, not an env slot). */
    CapSet cs;
    bool ok = collect_caps(t->as.shift.body, t->as.shift.k.id, &cs);
    const CapSet *caps = (ok && cs.n > 0) ? &cs : NULL;
    emit_lifted(ce, hname, LH_SHIFT_BODY, NULL, TY_INT, NULL, t->as.shift.body, NULL, caps);
    char *env = emit_cont_env(ce, hname, caps);   /* k-less env */
    /* shift0 does NOT reinstall the delimiting prompt; plain shift does.  The
     * shift node is spliced onto cur_k (its ->next); after dk_run has driven the
     * captured computation to completion it is dead, so reclaim it with
     * dk_free_node (a single-node free -- dk_free would walk into cur_k).  See
     * docs/archive/cps-delimited-dk-node-leak.md. */
    char snode[48];
    snprintf(snode, sizeof(snode), "__sd%d", id);
    ce_line(ce, "DK *%s = %s(1, %s, %s, %s);", snode,
            t->as.shift.shift0 ? "dk_shift0" : "dk_shift", hname, env, ce->cur_k);
    ce_line(ce, "int64_t %s_r = dk_run(%s, 0);", snode, snode);
    ce_line(ce, "dk_free_node(%s);", snode);
    ce_line(ce, "return %s_r;", snode);
    free(env);
}

/* E7: is this resume a TAIL resume -- the resumed value is delivered straight to
 * the handler case's return (KK_RET/KK_PROMPT), with nothing after?  Then the
 * handler does no post-resume work and dk_perform can trampoline it. */
static bool resume_is_tail(const CTerm *t) {
    const CTerm *b = t->as.resume.body;
    return b && b->kind == CT_APPCONT
        && (b->as.appcont.kont.kind == KK_RET || b->as.appcont.kont.kind == KK_PROMPT)
        && b->as.appcont.v.kind == CA_CVAR
        && b->as.appcont.v.cvar_id == t->as.resume.x.id;
}

/* E7: does a handler CASE body reduce (down its straight-line tail) to a single
 * TAIL resume?  Such a case is installed with dk_handler_tail and its resume
 * emits dk_tail_resume (yield to the entry driver), keeping deep recursion flat.
 * A branching or post-resume-working case is NOT a tail-resume -- it keeps the
 * inline dk_handler / dk_invoke path (correct, just not trampolined). */
static bool case_body_tail_resumes(const CTerm *t) {
    while (t) {
        switch (t->kind) {
            case CT_RESUME:  return resume_is_tail(t);
            case CT_LETVAL:  t = t->as.letval.body; break;
            case CT_LETPRIM: t = t->as.letprim.body; break;
            case CT_LETCALL: t = t->as.letcall.body; break;
            case CT_LETRAW:  t = t->as.letraw.body; break;
            default: return false;
        }
    }
    return false;
}

/* ---- C4: algebraic effects (handle / perform / resume) --------------- *
 * handle mirrors reset: lift the handle continuation to a DKFrame, install a
 * dk_handler (carrying the case as a DKHandler), and run the handled body
 * threading that handler prompt.  perform mirrors shift: lift the perform
 * continuation (a pure value transform) onto the chain and dk_perform against
 * the effect's tag; the handler runs the case, and resume = dk_invoke. */
static void emit_handle(CE *ce, const CTerm *t) {
    int id = (*ce->helper_ctr)++;
    char kname[256];
    snprintf(kname, sizeof(kname), "%s_hk%d", ce->fn_cn, id);
    /* Lift the handle continuation as a RESUME-FRAME (LH_RESUME_CONT), not a
     * RESET_CONT: the frame receives its downstream chain `__kont` AT RUN TIME
     * (dk_run_impl passes the node's own ->next) instead of reading a pointer
     * baked into its env at install time.  This is the spine-unification fix
     * for the two-spine problem (docs/archive/cps-multishot-nontail-resume-
     * inner-handle-drops-clause-rest.md): a baked env always named the ORIGINAL
     * enclosing chain, so every dk_copy_range copy of this frame still jumped
     * there -- escaping its delimiter and re-running the outer continuation
     * once per multi-shot resume.  A resume-frame copy threads the COPY's own
     * marker-terminated tail; only the ORIGINAL (whose next borrows the real
     * enclosing chain, below) reaches the real rest of the program.  The env
     * carries scalar captures only -- no `__k` slot. */
    CapSet cs;
    bool ok = collect_caps(t->as.handle.body, t->as.handle.x.id, &cs);
    const CapSet *caps = (ok && cs.n > 0) ? &cs : NULL;
    char *hxn = cvar_cname(ce, t->as.handle.x);
    emit_lifted(ce, kname, LH_RESUME_CONT, hxn, t->as.handle.x.ty, t->as.handle.x.type,
                t->as.handle.body, NULL, caps);
    free(hxn);
    char *hkenv = emit_cont_env(ce, kname, caps);   /* caps-only env */

    /* B3 part 2: a DYNAMIC with-handler installs the handler group from the
     * runtime handler table (dk_hgroup_from_table) instead of a static per-case
     * chain -- no case fns are emitted here (they were emitted at the handler
     * literal's creation site).  The base frame carries the enclosing handlers
     * (an effect the table does not handle propagates outward) exactly like the
     * static path. */
    if (t->as.handle.dyn) {
        char hchain_d[64];
        snprintf(hchain_d, sizeof(hchain_d), "__h%d", id);
        char *tblv = atom_str(ce, &t->as.handle.dyn_table);
        ce_line(ce, "DK *%s = __dk_reap_keep(dk_hgroup_from_table("
                    "(const tur_handler_table_t *)(intptr_t)%s, "
                    "dk_frame_resume_borrow(%s, %s, %s)));",
                hchain_d, tblv, kname, hkenv, ce->cur_k);
        free(tblv);
        free(hkenv);
        const char *save_d = ce->cur_k;
        const Type *save_ty_d = ce->cur_ty;
        ce->cur_k = arena_strdup(&g_arena, hchain_d, strlen(hchain_d));
        ce->cur_ty = t->as.handle.x.type;
        emit_term(ce, t->as.handle.delim);
        ce->cur_k = save_d;
        ce->cur_ty = save_ty_d;
        return;
    }

    /* Lift each handler case as its own DKHandler and chain a dk_handler per case
     * onto the continuation.  dk_perform searches the chain by effect tag, so N
     * chained handlers dispatch N effects; the innermost `next` is the handle
     * continuation frame carrying the enclosing continuation `k`. */
    uint32_t nc = t->as.handle.n_cases;
    char **cnames = (char **)calloc(nc ? nc : 1, sizeof(char *));
    char **cenvs  = (char **)calloc(nc ? nc : 1, sizeof(char *));  /* per-case env expr */
    for (uint32_t ci = 0; ci < nc; ci++) {
        cnames[ci] = (char *)malloc(256);
        snprintf(cnames[ci], 256, "%s_hc%d_%u", ce->fn_cn, id, ci);
        /* A handler case may capture enclosing scalars (beyond its params + k),
         * carried in a caps-only env (k arrives via subk). */
        CapSet ccs;
        bool cok = collect_caps_case(t->as.handle.cases[ci].case_body, &t->as.handle.cases[ci], &ccs);
        const CapSet *ccaps = (cok && ccs.n > 0) ? &ccs : NULL;
        /* E7: emit_resume yields (dk_tail_resume) ONLY for a case installed with
         * dk_handler_tail -- a DEEP tail-resume.  A SHALLOW case keeps the inline
         * dk_invoke (its dk_perform never queues an H->next delivery to trampoline).
         * Must agree with the per-case ctor decision below. */
        bool save_ctr = ce->case_tail_resume;
        ce->case_tail_resume = !t->as.handle.shallow
            && case_body_tail_resumes(t->as.handle.cases[ci].case_body);
        emit_lifted(ce, cnames[ci], LH_HANDLER_CASE, NULL, TY_INT, NULL,
                    t->as.handle.cases[ci].case_body, &t->as.handle.cases[ci], ccaps);
        ce->case_tail_resume = save_ctr;
        cenvs[ci] = emit_cont_env(ce, cnames[ci], ccaps);   /* k-less env */
    }

    char hchain[64];
    snprintf(hchain, sizeof(hchain), "__h%d", id);
    /* Build the chain inside-out: base is the continuation frame; wrap each case
     * as dk_handler(tag_i, case_i, case_env_i, <inner>).  A shallow handle
     * (handle-shallow, F2) wraps each case as dk_handler_shallow instead, so a
     * resume does not re-install it (the effect-side analogue of shift0). */
    const char *hctor = t->as.handle.shallow ? "dk_handler_shallow" : "dk_handler";
    Buf chain; buf_init(&chain);
    /* The base is the handle continuation frame.  Its `next` is the ACTUAL
     * enclosing chain (cur_k), borrowed -- not a done-terminated copy of its
     * handler markers.  One spine then serves everything the old two-spine
     * layout split: an effect this handle does NOT handle walks straight into
     * the real enclosing handlers (outward propagation), dk_perform's capture
     * boundary stops at the real H, and its H->next delivery runs the real
     * rest of the program exactly once -- where the marker copy dead-ended at
     * done and delivery had to happen via the frame's baked env jump, once per
     * resume (the multi-shot-across-nested-handle miscompile).  The frame is a
     * RESUME-FRAME: dk_run_impl hands it its own ->next, so the original
     * threads the borrowed enclosing chain and a reified copy threads the
     * copy's own tail.  borrow_next keeps this chain's dk_free out of the
     * enclosing chain (which has its own reap owner). */
    buf_printf(&chain, "dk_frame_resume_borrow(%s, %s, %s)", kname, hkenv, ce->cur_k);
    for (int ci = (int)nc - 1; ci >= 0; ci--) {
        int tag = effect_tag(t->as.handle.cases[ci].effect);
        /* E7: a deep case that reduces to a TAIL resume installs with
         * dk_handler_tail so dk_perform yields it to the entry driver (flat).
         * emit_resume for that case emits the matching dk_tail_resume. */
        const char *ctor = hctor;
        if (!t->as.handle.shallow
            && case_body_tail_resumes(t->as.handle.cases[ci].case_body))
            ctor = "dk_handler_tail";
        /* A RE-OPENING case delivers its own value through the real enclosing
         * chain (emit_lifted gave it dk_case_enclosing_real as `__kont` and
         * turned shift_mode off), so mark its handler node: dk_perform returns
         * the case's result verbatim instead of delivering H->next a second
         * time.  Mutually exclusive with dk_handler_tail by construction --
         * case_body_tail_resumes rejects any body containing an interior
         * CT_PERFORM, which is exactly what makes case_reopens true. */
        bool creopen = case_reopens(t->as.handle.cases[ci].case_body);
        Buf nxt; buf_init(&nxt);
        if (creopen) buf_puts(&nxt, "dk_case_delivers(");
        buf_printf(&nxt, "%s(%d, %s, %s, %.*s)", ctor, tag, cnames[ci], cenvs[ci],
                   (int)chain.len, chain.data);
        if (creopen) buf_putc(&nxt, ')');
        buf_free(&chain);
        chain = nxt;
    }
    buf_putc(&chain, '\0');
    /* Installed as cur_k and threaded in tail position (like reset); register the
     * self-contained chain for reaping at the outermost entry boundary
     * (docs/archive/cps-delimited-dk-node-leak.md).  Under the re-opening path,
     * stamp this handle's sibling case-handlers with a shared group id (dk_hgroup)
     * so dk_case_enclosing_real / dk_perform can tell them apart from an enclosing
     * handle's handlers once a re-install flattens the chain (else a re-opened
     * outer effect in a multi-suspension continuation escapes). */
    ce_line(ce, "DK *%s = __dk_reap_keep(dk_hgroup(%s));", hchain, chain.data);
    buf_free(&chain);
    free(hkenv);
    for (uint32_t ci = 0; ci < nc; ci++) { free(cnames[ci]); free(cenvs[ci]); }
    free(cnames); free(cenvs);

    const char *save = ce->cur_k;
    const Type *save_ty = ce->cur_ty;
    ce->cur_k = arena_strdup(&g_arena, hchain, strlen(hchain));
    ce->cur_ty = t->as.handle.x.type;    /* KK_PROMPT crossing type = handle result */
    emit_term(ce, t->as.handle.delim);   /* body runs threading the handler prompt */
    ce->cur_k = save;
    ce->cur_ty = save_ty;
}

/* Is the perform continuation trivial -- deliver the result straight to the
 * function's return continuation (a tail perform)?  Then no frame is needed. */
static bool perform_cont_trivial(const CTerm *t) {
    const CTerm *b = t->as.perform.body;
    return b && b->kind == CT_APPCONT
        && (b->as.appcont.kont.kind == KK_RET || b->as.appcont.kont.kind == KK_PROMPT)
        && b->as.appcont.v.kind == CA_CVAR
        && b->as.appcont.v.cvar_id == t->as.perform.x.id;
}

static void emit_perform(CE *ce, const CTerm *t) {
    int tag = effect_tag(t->as.perform.effect);
    /* effect value word: a 0-arg effect passes 0; a 1-arg effect passes the arg's
     * slot value directly; a multi-arg effect (n>1) heap-packs each arg's slot
     * word into an `int64_t[n]` and passes the array pointer -- the handler case
     * unpacks each param from it (emit_lifted).  The array is leaked with the DK
     * nodes (a multi-shot resume shares it read-only), matching the env-leak
     * discipline. */
    char *arg = NULL, *sa;
    if (t->as.perform.n > 1) {
        int aid = (*ce->helper_ctr)++;
        char av[64];
        snprintf(av, sizeof av, "__eargs%d", aid);
        ce_line(ce, "int64_t *%s = (int64_t *)malloc(%u * sizeof(int64_t));",
                av, (unsigned)t->as.perform.n);
        /* The arg array (and any boxed slot within it) is read by the handler via
         * slot_load(consume=false) and shared read-only across a multi-shot
         * resume, so it is never freed at a load site; reap it at the outermost
         * entry boundary (docs/archive/cps-effect-perform-carrier-leak.md). */
        ce_line(ce, "__dk_reap_ptr((intptr_t)%s);", av);
        for (uint32_t i = 0; i < t->as.perform.n; i++) {
            char *ai = atom_str(ce, &t->as.perform.args[i]);
            char *si = slot_store_reap(ce->ctx, t->as.perform.args[i].ty,
                                       t->as.perform.args[i].type, ai);
            ce_line(ce, "%s[%u] = %s;", av, i, si);
            free(si); free(ai);
        }
        Buf sb; buf_init(&sb);
        buf_printf(&sb, "(intptr_t)%s", av);
        buf_putc(&sb, '\0');
        sa = strdup(sb.data); buf_free(&sb);
    } else {
        /* argument: 0-arg effects pass 0; 1-arg pass the arg (subset). */
        arg = (t->as.perform.n >= 1)
            ? atom_str(ce, &t->as.perform.args[0]) : strdup("0");
        TypeKind argty = (t->as.perform.n >= 1) ? t->as.perform.args[0].ty : TY_NIL;
        const Type *argt = (t->as.perform.n >= 1) ? t->as.perform.args[0].type : NULL;
        /* A boxed (Tier-C) single arg leaks like the multi-arg array -- read
         * consume=false by the handler; reap it at the entry boundary. */
        sa = slot_store_reap(ce->ctx, argty, argt, arg);
    }

    if (perform_cont_trivial(t)) {
        ce_line(ce, "return dk_perform(%d, %s, %s);", tag, sa, ce->cur_k);
    } else if (perform_body_ok(t->as.perform.body)) {
        int id = (*ce->helper_ctr)++;
        char pname[256];
        snprintf(pname, sizeof(pname), "%s_pf%d", ce->fn_cn, id);
        char *pxn = cvar_cname(ce, t->as.perform.x);
        /* N6.3: collect the continuation's scalar captures; when present, carry
         * them in a heap env struct passed as the frame env. */
        CapSet cs;
        bool ok = collect_caps(t->as.perform.body, t->as.perform.x.id, &cs);
        const CapSet *caps = (ok && cs.n > 0) ? &cs : NULL;
        emit_lifted(ce, pname, LH_PERFORM_CONT, pxn, t->as.perform.x.ty, t->as.perform.x.type,
                    t->as.perform.body, NULL, caps);
        free(pxn);
        if (caps) {
            /* Allocate + populate the env from the enclosing locals (same names).
             * The struct is leaked with the DK nodes (a multi-shot resume shares
             * it read-only). */
            char envv[64];
            snprintf(envv, sizeof envv, "__pfe%d", id);
            ce_line(ce, "%s_env *%s = (%s_env *)malloc(sizeof(%s_env));", pname, envv, pname, pname);
            for (int i = 0; i < cs.n; i++) {
                char *cn = cs.b[i] ? name_for_binding(ce->ctx, cs.b[i]) : strdup(cs.cvname[i]);
                ce_line(ce, "%s->f%d = %s;", envv, i, cn);
                free(cn);
            }
            /* Shared read-only across multi-shot resumes; reaped at the outermost
             * entry boundary (docs/archive/cps-delimited-dk-node-leak.md). */
            ce_line(ce, "__dk_reap_ptr((intptr_t)%s);", envv);
            /* The perform continuation frame is spliced onto cur_k; dk_perform
             * copies the captured range and never frees the node we hand it, so it
             * must be reclaimed single-node (dk_free would walk into cur_k).  A
             * post-dk_perform `dk_free_node` is SKIPPED when the handler tail-
             * resumes (E7: dk_perform yields via longjmp and never returns here),
             * leaking the frame (104 bytes, cps-perform-cont-frame-leak-on-tail-
             * resume.md).  Register it for a single-node free at the outermost entry
             * boundary (__dk_reap_node: kind=0, a bare free that does not walk into
             * cur_k) -- reached on both the normal-return and the tail-resume-yield
             * paths -- exactly like the Track-B resume-frame sibling below. */
            ce_line(ce, "return dk_perform(%d, %s, __dk_reap_node(dk_frame(%s, (intptr_t)%s, %s)));",
                    tag, sa, pname, envv, ce->cur_k);
        } else {
            ce_line(ce, "return dk_perform(%d, %s, __dk_reap_node(dk_frame(%s, 0, %s)));",
                    tag, sa, pname, ce->cur_k);
        }
    } else {
        /* Track A: the continuation contains a NESTED control op (a further
         * perform) -- perform_cont_reset_ok admitted it.  Lift it as a RESUME-FRAME
         * (LH_RESUME_CONT): the frame's `next` is ce->cur_k so dk_perform finds the
         * handler and splices the reinstalled-handler tail as the frame's run-time
         * downstream chain `__kont`, which the helper threads (a nested perform
         * inside re-dispatches against __kont, and a plain KK_RET delivers via
         * dk_run(__kont, v)) -- so the value is delivered exactly once.  The env
         * carries only the body's captures (no __k -- __kont is the runtime rest). */
        int id = (*ce->helper_ctr)++;
        char pname[256];
        snprintf(pname, sizeof(pname), "%s_rf%d", ce->fn_cn, id);
        char *pxn = cvar_cname(ce, t->as.perform.x);
        CapSet cs;
        bool ok = collect_caps(t->as.perform.body, t->as.perform.x.id, &cs);
        const CapSet *caps = (ok && cs.n > 0) ? &cs : NULL;
        emit_lifted(ce, pname, LH_RESUME_CONT, pxn, t->as.perform.x.ty, t->as.perform.x.type,
                    t->as.perform.body, NULL, caps);
        free(pxn);
        char *envexpr = emit_cont_env(ce, pname, caps);   /* caps-only env */
        /* The dk_frame_resume node's ->next is ce->cur_k (an enclosing chain), so
         * it is a single spliced node dk_perform never frees.  Unlike the
         * straight-line perform-cont sibling above -- which dk_free_node's its
         * dk_frame node right after dk_perform settles -- the value here is
         * returned inline, and under a MULTI-SHOT resume the node may still be
         * needed after this dk_perform returns.  Register it for a single-node
         * free at the outermost entry boundary (__dk_reap_node: kind=0, a bare
         * free that does not walk into cur_k), matching the reset/handle
         * structural-node reaping discipline (docs/archive/cps-resume-frame-node-leak.md,
         * docs/archive/cps-delimited-dk-node-leak.md). */
        ce_line(ce, "return dk_perform(%d, %s, __dk_reap_node(dk_frame_resume(%s, %s, %s)));",
                tag, sa, pname, envexpr, ce->cur_k);
        free(envexpr);
    }
    free(sa);
    free(arg);
}

/* F3 (cps-async): is the await continuation trivial (deliver the awaited value
 * straight to the function's return continuation)?  Then no lifted frame. */
static bool await_cont_trivial(const CTerm *t) {
    const CTerm *b = t->as.await.body;
    return b && b->kind == CT_APPCONT
        && (b->as.appcont.kont.kind == KK_RET || b->as.appcont.kont.kind == KK_PROMPT)
        && b->as.appcont.v.kind == CA_CVAR
        && b->as.appcont.v.cvar_id == t->as.await.x.id;
}

/* F3 (cps-async): `await` as a heap-continuation shift.  Mirrors emit_perform,
 * but shifts to the entry root prompt with the fixed __tur_await_body runtime
 * helper as the shift body (the awaited future rides as the shift's env).  The
 * continuation (bind the awaited value, run the rest) is lifted exactly like a
 * perform continuation (LH_PERFORM_CONT) and threaded as the shift's tail. */
static void emit_await(CE *ce, const CTerm *t) {
    char *fut = atom_str(ce, &t->as.await.fut);
    char *fsa = slot_store(ce->ctx, t->as.await.fut.ty, t->as.await.fut.type, fut);
    if (await_cont_trivial(t)) {
        ce_line(ce, "return dk_run(dk_shift(DK_ROOT_TAG, __tur_await_body, (intptr_t)%s, %s), 0);",
                fsa, ce->cur_k);
    } else if (perform_body_ok(t->as.await.body)) {
        /* Straight-line continuation (F3.1): a value-transform frame whose result
         * dk_run delivers to the function's k (frame next = cur_k). */
        int id = (*ce->helper_ctr)++;
        char pname[256];
        snprintf(pname, sizeof(pname), "%s_aw%d", ce->fn_cn, id);
        char *pxn = cvar_cname(ce, t->as.await.x);
        CapSet cs;
        bool ok = collect_caps(t->as.await.body, t->as.await.x.id, &cs);
        const CapSet *caps = (ok && cs.n > 0) ? &cs : NULL;
        emit_lifted(ce, pname, LH_PERFORM_CONT, pxn, t->as.await.x.ty, t->as.await.x.type,
                    t->as.await.body, NULL, caps);
        free(pxn);
        if (caps) {
            char envv[64];
            snprintf(envv, sizeof envv, "__awe%d", id);
            ce_line(ce, "%s_env *%s = (%s_env *)malloc(sizeof(%s_env));", pname, envv, pname, pname);
            for (int i = 0; i < cs.n; i++) {
                char *cn = cs.b[i] ? name_for_binding(ce->ctx, cs.b[i]) : strdup(cs.cvname[i]);
                ce_line(ce, "%s->f%d = %s;", envv, i, cn);
                free(cn);
            }
            ce_line(ce, "return dk_run(dk_shift(DK_ROOT_TAG, __tur_await_body, (intptr_t)%s, "
                        "dk_frame(%s, (intptr_t)%s, %s)), 0);",
                    fsa, pname, envv, ce->cur_k);
        } else {
            ce_line(ce, "return dk_run(dk_shift(DK_ROOT_TAG, __tur_await_body, (intptr_t)%s, "
                        "dk_frame(%s, 0, %s)), 0);",
                    fsa, pname, ce->cur_k);
        }
    } else {
        /* F3 gap-2: a bounded full CPS continuation (a branch or a further
         * sequential await -- await_cont_reset_ok, checked at admission).  Lift
         * it as a RESUME-FRAME whose `next` is the ACTUAL enclosing chain,
         * borrowed -- the spine unification
         * (docs/archive/cps-await-cont-baked-env.md): the frame receives its
         * downstream at run time, so the shift's capture now extends past the
         * frame into the real chain (up to the root prompt) and the parked
         * continuation is fully self-contained -- resuming it replays a COPY of
         * the rest instead of jumping to a baked original, and an effect
         * performed in the resumed continuation can find the (copied) enclosing
         * handlers where the old dk_done() `next` dead-ended.  A KK_RET appcont
         * emits `dk_run(__kont, v)`, a nested await emits its own shift against
         * __kont, and the value is delivered exactly once.  No cps->cps tail
         * call reaches here (that evicts), so the number of nested dk_invoke
         * resumes is statically bounded -- no O(N) stack. */
        int id = (*ce->helper_ctr)++;
        char aname[256];
        snprintf(aname, sizeof(aname), "%s_ak%d", ce->fn_cn, id);
        char *axn = cvar_cname(ce, t->as.await.x);
        CapSet cs;
        bool ok = collect_caps(t->as.await.body, t->as.await.x.id, &cs);
        const CapSet *caps = (ok && cs.n > 0) ? &cs : NULL;
        emit_lifted(ce, aname, LH_RESUME_CONT, axn, t->as.await.x.ty, t->as.await.x.type,
                    t->as.await.body, NULL, caps);
        free(axn);
        char *envexpr = emit_cont_env(ce, aname, caps);   /* caps-only env */
        ce_line(ce, "return dk_run(dk_shift(DK_ROOT_TAG, __tur_await_body, (intptr_t)%s, "
                    "dk_frame_resume_borrow(%s, %s, %s)), 0);",
                fsa, aname, envexpr, ce->cur_k);
        free(envexpr);
    }
    free(fsa);
    free(fut);
}

static void emit_resume(CE *ce, const CTerm *t) {
    /* resume k v = dk_invoke((DK*)k, v); slot-load the result and continue. */
    char *kk = atom_str(ce, &t->as.resume.k);
    char *vv = atom_str(ce, &t->as.resume.v);
    /* E7: a TAIL resume directly inside a handler case yields to the entry driver
     * (dk_tail_resume) instead of resuming inline (dk_invoke) -- the trampoline
     * keeps deep effectful recursion flat.  dk_perform installed this case with
     * dk_handler_tail (case_body_tail_resumes agrees), so it queued the H->next
     * delivery and this yield hands off the resumed chain; the value is delivered
     * by the driver, so nothing follows here. */
    /* (cont? k) support: mark k consumed at the user resume site so a later
     * `cont?` on the same k reads false (matches the fiber path). */
    ce_line(ce, "((DK *)(%s))->consumed = 1;", kk);
    if (ce->handler_case_mode && ce->case_tail_resume && resume_is_tail(t)) {
        char *sv = slot_store_reap(ce->ctx, t->as.resume.v.ty, t->as.resume.v.type, vv);
        ce_line(ce, "return dk_tail_resume((DK *)(%s), %s);", kk, sv);
        free(sv); free(kk); free(vv);
        return;
    }
    /* resume value -> slot; a Tier C box is registered for reap (the resumed
     * continuation may be re-run, so it is read consume=false and cannot be freed
     * at the load -- the reap list owns it). */
    char *sv = slot_store_reap(ce->ctx, t->as.resume.v.ty, t->as.resume.v.type, vv);
    Buf inv; buf_init(&inv);
    buf_printf(&inv, "dk_invoke((DK *)(%s), %s)", kk, sv);
    buf_putc(&inv, '\0');
    /* result <- slot; reaped Tier C box (the resumed continuation may be re-run). */
    char *ld = slot_load(ce->ctx, t->as.resume.x.ty, t->as.resume.x.type, inv.data, false);
    char *bn = cvar_cname(ce, t->as.resume.x);
    ce_line(ce, "%s = %s;", bn, ld);
    buf_free(&inv);
    free(bn); free(ld); free(sv); free(kk); free(vv);
    /* cps-body-panic-not-propagated: the resumed continuation may have
     * panicked under a handler and signalled back through dk_invoke; the rest
     * of this handler case is past the panic. */
    cps_panic_check(ce);
    emit_term(ce, t->as.resume.body);
}

/* CT_CALLCC (U7): (call/cc f)/(escape f) as a LOCAL setjmp escape landing --
 * a native port of emit_cps_callcc.  Establish a tur_escape_cont landing, run
 * the (capture-free) receiver f with the landing handle as its continuation,
 * and bind x to f's normal return; an upward (tur_escape_resume &cc v) longjmps
 * back and delivers v instead.  Does NOT thread the DK continuation, so the
 * enclosing prompt chain is untouched.  The escape runtime (tur_escape_cont /
 * tur_escape_resume) is emitted by emit_cps_callcc_prelude, gated on the
 * program containing EX_CALLCC (fires regardless of backend). */
static void emit_callcc(CE *ce, const CTerm *t) {
    const Expr *e = t->as.callcc.e;
    const Expr *f = e->as.callcc_.fn;
    while (f && f->kind == EX_ASCRIBE) f = f->as.ascribe_.inner;   /* peel like build_callcc */
    char *xn = cvar_cname(ce, t->as.callcc.x);
    const char *rty = binder_ctype_full(ce->ctx, t->as.callcc.x.ty, t->as.callcc.x.type);
    int id = (*ce->helper_ctr)++;
    char cc[48];
    snprintf(cc, sizeof(cc), "__cc%d", id);

    /* saffron-dynamic-surface-pass (low): an `any`-typed call/cc.  The
     * binder is a 16-byte `tur_tagged_t`, which cannot be C-cast to (it is a
     * struct) and does not fit the landing's int64 `result`: the receiver
     * returns it directly on the normal path, and on the resumed path (k v)
     * delivered a heap copy's address (__tur_escape_box_any) that is read and
     * freed here. */
    bool boxed = rty && strcmp(rty, "tur_tagged_t") == 0;
    ce_line(ce, "tur_escape_cont %s;", cc);
    ce_line(ce, "%s.valid = 1;", cc);
    ce_line(ce, "tur_escape_live_push(&%s);", cc);
    ce_line(ce, "if (TUR_SETJMP(%s.buf) == 0) {", cc);
    ce->indent += 4;
    /* Normal path: emit the receiver value, then call it with &cc. */
    int saved = ce->ctx->indent;
    ce->ctx->indent = ce->indent;
    char *fval = emit_value(ce->ctx, ce->out, f);
    ce->ctx->indent = saved;
    if (f->kind == EX_CLOSURE) {
        struct Closure *closure = f->as.closure_.closure;
        char *thunk_name;
        if (closure->fn->binding) {
            thunk_name = raw_name_for_binding(closure->fn->binding);
        } else {
            thunk_name = malloc(64);
            snprintf(thunk_name, 64, "__fn_anon_%d",
                     closure->fn->n_params > 0 ? closure->fn->params[0]->id : 0);
        }
        if (boxed)
            ce_line(ce, "%s = %s(%s, (int64_t)(intptr_t)&%s);", xn, thunk_name, fval, cc);
        else
            ce_line(ce, "%s = (%s)%s(%s, (int64_t)(intptr_t)&%s);", xn, rty, thunk_name, fval, cc);
        free(thunk_name);
    } else {
        if (boxed)
            ce_line(ce, "%s = %s((int64_t)(intptr_t)&%s);", xn, fval, cc);
        else
            ce_line(ce, "%s = (%s)%s((int64_t)(intptr_t)&%s);", xn, rty, fval, cc);
    }
    /* f returned normally: the captured continuation is now dead. */
    ce_line(ce, "%s.valid = 0;", cc);
    ce_line(ce, "tur_escape_live_pop(&%s);", cc);
    free(fval);
    ce->indent -= 4;
    ce_line(ce, "} else {");
    ce->indent += 4;
    /* Resumed path: an upward (tur_escape_resume &cc v) delivered v here.
     * The prompt is done either way; the pop also drops every prompt the
     * escape jumped over. */
    ce_line(ce, "%s.valid = 0;", cc);
    ce_line(ce, "tur_escape_live_pop(&%s);", cc);
    if (boxed) {
        ce_line(ce, "{ tur_tagged_t *__ccb = (tur_tagged_t *)(intptr_t)%s.result; "
                    "%s = *__ccb; free(__ccb); }", cc, xn);
    } else {
        ce_line(ce, "%s = (%s)%s.result;", xn, rty, cc);
    }
    ce->indent -= 4;
    ce_line(ce, "}");
    free(xn);
    emit_term(ce, t->as.callcc.body);
}

/* ---- signatures ------------------------------------------------------ */

/* The by-value C type spelling for param `i` of `fd` -- the __cps ABI spelling.
 * Extracted so the direct->cps entry wrapper can reuse it for the params it does
 * NOT rewrite to const-pointer (see cps_entry_param_by_ptr). */
static const char *emit_param_ctype(EmitCtx *ctx, const FnDef *fd, uint32_t i) {
    /* A rank-2 poly fn value crosses as a fat closure (`tur_poly_fn_t`),
     * matching the direct emitter's signature -- not the `void *` its scalar-
     * pointer type kind would otherwise pick.  A delegated indirect call
     * (`fnv.fn` / `fnv.env`) reads the struct's fields, so the parameter must
     * be declared as the fat closure, not an opaque pointer. */
    if (fd->params[i]->is_poly_fn)
        return "tur_poly_fn_t";
    if (fd->params[i]->type.kind == TY_FN) {
        /* A plain (non-rank-2) fn param crosses as the direct emitter's
         * function-pointer spelling: a typed C-ABI pointer becomes its
         * registered `R (*)(A...)` typedef, an ordinary closure carrier the
         * opaque `int64_t`.  type_c_name(TY_FN) leaks a bad spelling, so this
         * mirrors emit_module.c's signature emission exactly -- otherwise a
         * delegated call through the param (which the direct emitter emits)
         * and the __cps declaration would disagree. */
        const char *td = fd->params[i]->type.as.fn.cfnptr
            ? register_fn_ptr_typedef(&fd->params[i]->type) : NULL;
        return td ? td : "int64_t";
    }
    /* A by-value aggregate param needs its real (monomorphized) C type,
     * which emit_type_from_kind(TY_ADT/APP/STRUCT) loses -- use the full
     * Type. */
    return binder_ctype_full(ctx, fd->params[i]->type.kind, &fd->params[i]->type);
}

static void emit_params(EmitCtx *ctx, Buf *file, const FnDef *fd) {
    for (uint32_t i = 0; i < fd->n_params; i++) {
        if (i) buf_puts(file, ", ");
        const char *pty = emit_param_ctype(ctx, fd, i);
        char *pn = name_for_binding(ctx, fd->params[i]);
        buf_printf(file, "%s %s", pty, pn);
        free(pn);
    }
}

/* True iff the direct emitter / forward decl passes param `i` of `fd` by const
 * pointer (a large by-value product / record ADT -- type_struct_pass_by_ptr).
 * The direct->cps entry wrapper is the plain-name boundary symbol that direct
 * (uncolored) callers reach, so its C signature must mirror the direct forward
 * decl (emit_module.c emit_fn_forward_decls, `const <type_c_name> *`).  Its
 * `__cps` body keeps the by-value spelling (emit_param_ctype), so the wrapper
 * dereferences the pointer when threading the arg into the `__cps` call.  Spell
 * such a param by value in the wrapper and it clashes with the const-pointer
 * forward decl ("conflicting types for '<fn>'").  Mirrors emit_module.c:
 * emit_fn_forward_decls and emit_fns.c's direct param emission exactly. */
static bool cps_entry_param_by_ptr(const Expr *e, const FnDef *fd, uint32_t i) {
    if (fd->params[i]->is_poly_fn) return false;
    if (fd->params[i]->type.kind == TY_FN) return false;
    if (fd->params[i]->is_fat && fd->body && fd->body->kind == EX_INLINE_C)
        return false;
    Type pty = (!fd->params[i]->is_fat && e->type.as.fn.arg_full_types
                && e->type.as.fn.arg_full_types[i])
        ? *e->type.as.fn.arg_full_types[i]
        : fd->param_types[i];
    bool inline_c = (fd->body && fd->body->kind == EX_INLINE_C);
    return !fd->closure && !inline_c && type_struct_pass_by_ptr(pty);
}

/* The `type_c_name` spelling used inside the `const %s *` for a by-ptr wrapper
 * param -- must match emit_fn_forward_decls' `type_c_name(_fwd_pty)`. */
static const char *cps_entry_param_byptr_ctype(const Expr *e, const FnDef *fd,
                                               uint32_t i) {
    Type pty = (!fd->params[i]->is_fat && e->type.as.fn.arg_full_types
                && e->type.as.fn.arg_full_types[i])
        ? *e->type.as.fn.arg_full_types[i]
        : fd->param_types[i];
    return type_c_name(pty);
}

/* Emit forward declarations for every emittable __cps function, once per
 * program.  Goes into `file` (appended after the DK prelude in `out`). */
static void emit_forward_decls(EmitCtx *ctx, Buf *file) {
    for (size_t i = 0; i < g_ents_n; i++) {
        if (!g_ents[i].in_s) continue;
        const FnDef *fd = g_ents[i].fd;
        /* r7rs-programs-compile-slowly: the library unit of a split build
         * declares only what it defines -- the stdlib's. */
        if (g_emit_split == EMIT_SPLIT_LIB && !emit_split_lib_owns(fd->binding)) continue;
        char *cn = raw_name_for_binding(fd->binding);
        if (g_emit_split == EMIT_SPLIT_LIB) {
            size_t cl = strlen(cn);
            char *xn = (char *)malloc(cl + 6);
            if (xn) {
                memcpy(xn, cn, cl);
                memcpy(xn + cl, "__cps", 6);
                emit_split_note_export(xn);
                free(xn);
            }
        }
        /* r7rs-programs-compile-slowly: a split build's library unit defines
         * a stdlib defn's CPS body with external linkage, for a colored
         * caller in the client unit. */
        buf_printf(file, "%sint64_t %s__cps(",
                   emit_split_lib_owns(fd->binding) ? "" : "static ", cn);
        emit_params(ctx, file, fd);
        if (fd->n_params) buf_puts(file, ", ");
        buf_puts(file, "DK *__kont);\n");
        free(cn);
    }
    /* G3b: forward-declare each mono-template monomorph's `<clone>__cps`, since a
     * cps->cps caller (a DK peer) references it before the spec-body emit loop
     * defines it.  emit_params + the return type resolve to concrete types under
     * the active spec. */
    for (size_t i = 0; i < g_ents_n; i++) {
        if (!g_ents[i].mono_template) continue;
        FnDef *fd = (FnDef *)g_ents[i].fd;
        if (g_emit_split == EMIT_SPLIT_LIB && !emit_split_lib_owns(fd->binding)) continue;
        for (uint32_t s = 0; s < ctx->n_abi_specializations; s++) {
            EmitAbiSpecialization *spec = &ctx->abi_specializations[s];
            if (spec->fn != fd || !spec->clone_name) continue;
            const EmitAbiSpecialization *saved = ctx->current_abi_specialization;
            ctx->current_abi_specialization = spec;
            g_cps_mono_resolver = ctx;
            buf_printf(file, "static int64_t %s__cps(", spec->clone_name);
            emit_params(ctx, file, fd);
            if (fd->n_params) buf_puts(file, ", ");
            buf_puts(file, "DK *__kont);\n");
            g_cps_mono_resolver = NULL;
            ctx->current_abi_specialization = saved;
        }
    }
}

/* Drop the program-keyed caches (see the declaration in emit_cps_ir.h). */
void emit_cps_ir_forget(void) {
    if (g_arena_live) { arena_free(&g_arena); g_arena_live = false; }
    free(g_ents); g_ents = NULL; g_ents_n = 0;
    g_prog = NULL;
    g_ents_ctx = NULL;
    fdc_prog = NULL;
    g_fwd_done = false;
    g_eff_n = 0;
    ctg_reset();
}

bool emit_cps_ir_program_has_emittable(const Expr *program) {
    ensure_S(program);
    for (size_t i = 0; i < g_ents_n; i++)
        if (g_ents[i].in_s) return true;
    return false;
}

/* Whether the CT-IR CPS backend emits `b`'s function as `<b>__cps(..., DK*)`.
 * The legacy CPS3 `--cps-path` wrapper path (emit_module.c / emit_fns.c) uses
 * this to skip a colored function the cps-backend already emits -- otherwise the
 * two mechanisms declare the same `__cps` symbol with different signatures
 * (`void(tur_cps_cont_t*, ...)` vs `int64_t(..., DK*)`), a `conflicting types`
 * error.  Post-graduation the cps-backend is always-on, so this guard is what
 * keeps `--cps-path` from colliding with it. */
bool emit_cps_ir_emits_binding(const Expr *program, const Binding *b) {
    if (!program || !b) return false;
    ensure_S(program);
    return binding_in_s(b);
}

/* G1 (cps-backend-generic-monomorph-classification-plan): analysis-only probe.
 * When --dump-cps-mono is set and this call is emitting a colored-generic
 * MONOMORPH body (ctx->current_abi_specialization pins fd to concrete type args),
 * report whether that monomorph's concrete signature + body would land in the
 * CPS-backend subset -- the information the generic template cannot give (it
 * sig-rejects on the tyvar TY_APP and is never a candidate).  Substitution is via
 * emit_resolve_type through the active spec (the g_cps_mono_resolver hook that
 * slot_ok_t consults).  Changes no emitted code; a pure `stderr` report. */
/* G2: monomorph signature admissibility using the direct emitter's MATERIALIZED
 * concrete types (spec->result_type / arg_types[]) rather than re-resolving the
 * generic annotation.  emit_resolve_type can collapse `(Box A)` to the bare,
 * unapplied `TY_ADT Box` that the by-value-product gate rejects; the spec types
 * are the fully-applied `TY_APP (Box int)` the gate recognizes.  Mirrors
 * fn_sig_ok's per-binding checks (is_poly_fn / is_borrow / effect-free fn param /
 * name clash) but reads the concrete type at each position. */
static bool mono_sig_ok(const FnDef *fd, const EmitAbiSpecialization *spec) {
    /* fn-value-fat-normalization (effect-row increment): a monomorph SPEC
     * clone of a lifted CLOSURE lambda is never CPS-emitted.  The spec's env
     * layout rides an env_name_override the CPS render's base-env unpack does
     * not know, and the registry story only ever threads the BASE entry (the
     * fat box's slot 0).  The clone keeps the direct path. */
    if (fd->closure) return false;
    const Type *rt = (spec->result_type.kind != TY_UNKNOWN)
                   ? &spec->result_type : fn_ret_type(fd);
    /* A CONCRETE monomorph of an ORDINARY defn has a single concrete C signature
     * (its clone name is unique to this specialization), so a by-value-aggregate
     * param / return -- which the scalar-only `sig_slot_ok` rejects for the shared
     * generic base name -- IS spellable: admit it via the wider `slot_ok_t` gate.
     * This makes a by-value-ADT-signature defn (`option-eq? : (Option A) params`)
     * a mono-template so its callers thread the mono `__cps` (cps->cps, which
     * crosses the by-value arg) instead of a cps->direct call that cannot.
     *
     * A TYPECLASS-INSTANCE method (`spec->typeclass_inst != NULL`) is the ONE
     * exception: its clone is ALSO emitted with the uniform CARRIER ABI
     * (`_Bool(int64_t, int64_t)`) for the dict-dispatch path, so a concrete
     * by-value CPS re-emission (`_Bool(tur_adt_Map__X *, ...)`) collides
     * (`conflicting types for <clone>`).  Keep the strict scalar-only gate for
     * instance methods so they stay SIG-REJECT (no CPS clone, no collision). */
    bool is_inst = spec && spec->typeclass_inst != NULL;
    /* Widen ONLY to a by-value FLAT PRODUCT (`slot_box_ty`: an owning-free Tier-C
     * aggregate boxed into the slot, e.g. `tur_adt_Option__int`), NOT to a
     * heap-ADT/struct HANDLE (`carrier_handle_ok`).  A heap handle threaded
     * through the concrete mono `__cps` mishandles its interior carrier fields
     * (show-collections: a Map with cstr keys printed the key POINTERS), so those
     * stay on the strict scalar gate.  A scalar always passes `sig_slot_ok`. */
    /* ... and an ERASED carrier app (slot_erased_carrier_app): the clone the
     * ABI scan mints for an UNPINNED call of a colored generic, whose only
     * lowering this is.  See colored-generic-tyvar-elemented-param-sig-rejects.
     * NOT when the clone's RESULT is a by-value aggregate: that mixed shape
     * (`result_map__spec__tur_adt_Result__int__int_int64_t_int64_t`, erased
     * receiver in, `(Result int int)` struct out) delivers the delegated
     * match's struct temp straight into the int64 return slot unboxed
     * (`__t0 = __t348`, a cc error in typed/result-basic), so it keeps the
     * gate it had. */
    bool erased_arg_ok = !is_inst && !(rt->kind != TY_NIL && slot_box_ty(rt));
    #define MONO_SLOT_OK(t, k) (is_inst ? sig_slot_ok((t), (k)) \
                                        : (sig_slot_ok((t), (k)) || slot_box_ty(t) || \
                                           slot_carrier_app(t) || \
                                           (erased_arg_ok && slot_erased_carrier_app(t))))
    if (rt->kind != TY_NIL && !MONO_SLOT_OK(rt, rt->kind)) return false;
    for (uint32_t i = 0; i < fd->n_params; i++) {
        const Binding *p = fd->params[i];
        const Type *pt = (i < spec->n_args) ? &spec->arg_types[i] : &p->type;
        if (p->is_poly_fn) return false;
        bool fn_param_ok = pt->kind == TY_FN;
        if (!p->is_borrow && !fn_param_ok
            && !MONO_SLOT_OK(pt, pt->kind)) return false;
        if (param_name_clashes_cps(p)) return false;
    }
    #undef MONO_SLOT_OK
    return true;
}

static void cps_dump_mono_admissible(EmitCtx *ctx, FnDef *fd) {
    const EmitAbiSpecialization *spec = ctx->current_abi_specialization;
    if (!spec || spec->fn != fd || !fd->body) return;
    if (!fd->cps_colored) return;   /* only colored generics are interesting */
    Arena tmp; arena_init(&tmp, 1 << 16);
    CTerm *t = cps_ir_translate_fn(&tmp, (Expr *)ctx->program_root, fd);
    /* Signature: materialized spec types (no resolver hook needed -- they are
     * already concrete).  Body: the generic binders are tyvar-typed, so the
     * resolver hook substitutes each through the active spec while term_core_ok
     * runs. */
    bool sig  = mono_sig_ok(fd, spec);
    g_cps_mono_resolver = ctx;
    bool body = t && term_core_ok(t);
    g_cps_mono_resolver = NULL;
    const Symbol *handled0[1];
    bool island = t && is_cps_island(t, handled0, 0);
    const char *nm = spec->clone_name ? spec->clone_name
                   : (fd->binding && fd->binding->name ? fd->binding->name->name : "?");
    bool admissible = sig && body;
    fprintf(stderr, "[cps-mono] %s: sig=%s body=%s island=%s => %s\n",
            nm, sig ? "ok" : "no", body ? "ok" : "no", island ? "yes" : "no",
            admissible ? (island ? "ISLAND-EMITTABLE" : "ADMISSIBLE(cross-fn)") : "fallback");
    arena_free(&tmp);
}

/* ---- cps-self-tail-call-relies-on-sibling-call: the group registry ---- */
static CpsTcg g_ctg[32];
static int    g_n_ctg;
static bool   g_ctg_built;
static int    g_ctg_ctr;

static void ctg_reset(void) {
    for (int i = 0; i < g_n_ctg; i++) {
        for (uint32_t k = 0; k < g_ctg[i].n_slots; k++) free(g_ctg[i].slot_ctype[k]);
        for (int m = 0; m < g_ctg[i].n_mem; m++) free(g_ctg[i].body[m]);
    }
    memset(g_ctg, 0, sizeof g_ctg);
    g_n_ctg = 0;
    g_ctg_built = false;
}

/* Every colored tail call's callee in `t`, lifted regions included -- an
 * over-approximation of the edges, which only ever proposes a group; whether
 * a given call becomes a jump is decided where it is rendered. */
static void ctg_tail_targets(const CTerm *t, const FnDef **out, int *n, int cap) {
    if (!t || *n >= cap) return;
    switch (t->kind) {
        case CT_TAILCALL:
            if (t->as.tailcall.fn && !t->as.tailcall.via_registry &&
                !t->as.tailcall.via_fncps && t->as.tailcall.kont.kind == KK_RET) {
                const FnDef *g = fd_for_binding(g_prog, t->as.tailcall.fn);
                if (!g) return;
                for (int i = 0; i < *n; i++) if (out[i] == g) return;
                out[(*n)++] = g;
            }
            return;
        case CT_LETRAW:  ctg_tail_targets(t->as.letraw.body, out, n, cap); return;
        case CT_LETVAL:  ctg_tail_targets(t->as.letval.body, out, n, cap); return;
        case CT_LETPRIM: ctg_tail_targets(t->as.letprim.body, out, n, cap); return;
        case CT_LETCALL: ctg_tail_targets(t->as.letcall.body, out, n, cap); return;
        case CT_LETCONT:
            ctg_tail_targets(t->as.letcont.jbody, out, n, cap);
            ctg_tail_targets(t->as.letcont.body, out, n, cap);
            return;
        case CT_IF:
            ctg_tail_targets(t->as.if_.then_, out, n, cap);
            ctg_tail_targets(t->as.if_.else_, out, n, cap);
            return;
        case CT_MATCH:
            for (uint32_t i = 0; i < t->as.match.n_arms; i++)
                ctg_tail_targets(t->as.match.arms[i].body, out, n, cap);
            return;
        case CT_PERFORM: ctg_tail_targets(t->as.perform.body, out, n, cap); return;
        case CT_AWAIT:   ctg_tail_targets(t->as.await.body, out, n, cap); return;
        case CT_RESUME:  ctg_tail_targets(t->as.resume.body, out, n, cap); return;
        case CT_CALLCC:  ctg_tail_targets(t->as.callcc.body, out, n, cap); return;
        default: return;
    }
}

/* May `fd` be a group member?  The same bar the self backedge sets (see
 * emit_cps_ir_try_fn), asked of the classification table before any member
 * is rendered.  The cell / loop-carried scans are the ones the render runs;
 * their tables are per-render globals, emptied again here. */
static bool ctg_member_ok(const SEnt *se) {
    if (!se || !se->in_s || se->mono_template || !se->term) return false;
    const FnDef *fd = se->fd;
    if (!fd || !fd->binding || fd->closure) return false;
    if (fn_is_main(fd) || fn_is_d2b_main(fd)) return false;
    if (emit_split_lib_owns(fd->binding)) return false;
    g_byref_muts_n = 0;
    g_loop_carried_n = 0;
    loop_carried_scan(se->term);
    byref_scan(se->term);
    bool ok = true;
    for (uint32_t i = 0; ok && i < fd->n_params; i++)
        ok = fd->params[i] && !fd->params[i]->is_poly_fn &&
             !is_byref_mut(fd->params[i]) && !is_loop_carried(fd->params[i]);
    g_byref_muts_n = 0;
    g_loop_carried_n = 0;
    return ok;
}

#define CTG_EXPLORE 64
static void ctg_build(EmitCtx *ctx) {
    if (g_ctg_built) return;
    g_ctg_built = true;
    for (size_t r = 0; r < g_ents_n && g_n_ctg < 32; r++) {
        const SEnt *root = &g_ents[r];
        if (!ctg_member_ok(root)) continue;
        bool grouped = false;
        for (int gi = 0; gi < g_n_ctg && !grouped; gi++)
            if (ctg_member_idx(&g_ctg[gi], root->fd) >= 0) grouped = true;
        if (grouped) continue;
        /* Forward reachability over eligible members from `root`. */
        const SEnt *node[CTG_EXPLORE];
        bool adj[CTG_EXPLORE][CTG_EXPLORE];
        memset(adj, 0, sizeof adj);
        int nn = 0;
        node[nn++] = root;
        for (int i = 0; i < nn; i++) {
            const FnDef *cs[CTG_EXPLORE];
            int nc = 0;
            ctg_tail_targets(node[i]->term, cs, &nc, CTG_EXPLORE);
            for (int c = 0; c < nc; c++) {
                SEnt *ce_ = ent_of(cs[c]);
                if (!ce_ || ce_ == node[i]) continue;
                int j = -1;
                for (int k = 0; k < nn; k++) if (node[k] == ce_) j = k;
                if (j < 0) {
                    bool in_other = false;
                    for (int gi = 0; gi < g_n_ctg && !in_other; gi++)
                        if (ctg_member_idx(&g_ctg[gi], ce_->fd) >= 0) in_other = true;
                    if (in_other || !ctg_member_ok(ce_) || nn >= CTG_EXPLORE) continue;
                    j = nn;
                    node[nn++] = ce_;
                }
                adj[i][j] = true;
            }
        }
        /* The component: the reached nodes that also reach back to `root`. */
        bool back[CTG_EXPLORE] = { false };
        back[0] = true;
        for (bool changed = true; changed; ) {
            changed = false;
            for (int i = 0; i < nn; i++) {
                if (back[i]) continue;
                for (int j = 0; j < nn; j++)
                    if (adj[i][j] && back[j]) { back[i] = true; changed = true; break; }
            }
        }
        const SEnt *mem[CTG_EXPLORE];
        int nm = 0;
        for (int i = 0; i < nn; i++) if (back[i]) mem[nm++] = node[i];
        if (nm < 2 || nm > CTG_MAXMEM) continue;
        uint32_t n_slots = 0;
        for (int i = 0; i < nm; i++) n_slots += mem[i]->fd->n_params;
        if (n_slots > CTG_MAXSLOTS) continue;
        CpsTcg *g = &g_ctg[g_n_ctg++];
        memset(g, 0, sizeof *g);
        snprintf(g->name, sizeof g->name, "__cps_tcg_%d", g_ctg_ctr++);
        g->n_mem = nm;
        for (int i = 0; i < nm; i++) {
            g->mem[i] = mem[i]->fd;
            g->slot_base[i] = g->n_slots;
            for (uint32_t k = 0; k < mem[i]->fd->n_params; k++)
                g->slot_ctype[g->n_slots++] = strdup(emit_param_ctype(ctx, mem[i]->fd, k));
        }
    }
}

static CpsTcg *ctg_of(const FnDef *fd, int *idx) {
    for (int gi = 0; gi < g_n_ctg; gi++) {
        int m = ctg_member_idx(&g_ctg[gi], fd);
        if (m >= 0) { *idx = m; return &g_ctg[gi]; }
    }
    return NULL;
}

static void ctg_emit_signature(Buf *out, const CpsTcg *g, bool named) {
    buf_printf(out, "static int64_t %s(int%s", g->name, named ? " __tcg_st" : "");
    for (uint32_t k = 0; k < g->n_slots; k++) {
        if (named) buf_printf(out, ", %s __tcg_s%u", g->slot_ctype[k], (unsigned)k);
        else       buf_printf(out, ", %s", g->slot_ctype[k]);
    }
    buf_puts(out, named ? ", DK *__kont)" : ", DK *)");
}

/* The fused function.  A member whose body was never rendered here -- not
 * expected, but not ruled out by anything this file controls -- keeps its
 * case as a call to its own `__cps`, which is then its original definition
 * rather than a wrapper, so the case can never call back into itself. */
static void ctg_emit_def(Buf *file, CpsTcg *g) {
    if (g->defined) return;
    g->defined = true;
    ctg_emit_signature(file, g, true);
    buf_puts(file, " {\n");
    buf_puts(file, "    __tcg_top:;\n");
    buf_puts(file, "    switch (__tcg_st) {\n");
    for (int m = 0; m < g->n_mem; m++) {
        buf_printf(file, "    case %d: {\n", m);
        if (g->body[m]) {
            buf_puts(file, g->body[m]);
        } else {
            char *cn = raw_name_for_binding(g->mem[m]->binding);
            buf_printf(file, "        return %s__cps(", cn);
            for (uint32_t k = 0; k < g->mem[m]->n_params; k++)
                buf_printf(file, "__tcg_s%u, ", (unsigned)(g->slot_base[m] + k));
            buf_puts(file, "__kont);\n");
            free(cn);
        }
        buf_puts(file, "    }\n");
    }
    buf_puts(file, "    default: break;\n    }\n    return 0;\n}\n");
}

/* End of a translation unit: define any group whose last member was never
 * rendered (its rendered members' wrappers already call it). */
void emit_cps_ir_flush_groups(Buf *file) {
    for (int gi = 0; gi < g_n_ctg; gi++)
        if (g_ctg[gi].declared && !g_ctg[gi].defined) ctg_emit_def(file, &g_ctg[gi]);
}

static bool emit_cps_ir_try_fn_impl(EmitCtx *ctx, Buf *file, const Expr *e);
bool emit_cps_ir_try_fn(EmitCtx *ctx, Buf *file, const Expr *e) {
    /* Mark the DK body for the direct emitter it delegates to: a value it
     * puts on the C stack (a stack fat box) may be read by a continuation
     * that runs after this frame has returned. */
    int prev = ctx ? ctx->in_cps_fn : 0;
    if (ctx) ctx->in_cps_fn = prev + 1;
    bool r = emit_cps_ir_try_fn_impl(ctx, file, e);
    if (ctx) ctx->in_cps_fn = prev;
    return r;
}
static bool emit_cps_ir_try_fn_impl(EmitCtx *ctx, Buf *file, const Expr *e) {
    if (g_dump_cps_mono && e && e->kind == EX_FN_DEF && e->as.fn_def_.fn
        && ctx->current_abi_specialization)
        cps_dump_mono_admissible(ctx, e->as.fn_def_.fn);
    if (!e || e->kind != EX_FN_DEF || !e->as.fn_def_.fn) return false;
    FnDef *fd = e->as.fn_def_.fn;
    if (!fd->binding) return false;
    const Expr *program = ctx->program_root;
    if (!program) return false;
    cps_deferred_reset();   /* binder ids are per function; never match across */

    /* ensure_S colors the program (idempotently) and classifies it; the
     * pipeline has not colored by emit time, so this must precede any read of
     * fd->cps_colored / g_ents.  g_emit_ctx gives it the complete spec set
     * (ctx->abi_specializations) for the G3b mono-template classification. */
    g_emit_ctx = ctx;
    ensure_S(program);
    SEnt *se = ent_of(fd);

    /* G3a: a colored-generic MONOMORPH emitted as a self-contained DK island.
     * The generic template sig-rejects (tyvar TY_APP) so `se->in_s` is false, but
     * this specific monomorph -- concrete signature admissible, body core, and a
     * self-contained island (is_cps_island: every effect handled within, no
     * colored callee) -- is safe to CPS-emit standalone: its entry wrapper keeps
     * the SAME `<clone>` symbol callers already call, and its effects never touch
     * another function's machine.  No taint/routing change. */
    const EmitAbiSpecialization *spec = ctx->current_abi_specialization;
    bool island_mono = false;   /* G3a: self-contained island (callers use wrapper) */
    bool mono_emit = false;     /* G3a or G3b: this monomorph is CPS-emitted */
    if (spec && spec->fn == fd && fd->cps_colored && se && se->term
        && spec->clone_name) {
        g_cps_mono_resolver = ctx;
        bool ok = mono_sig_ok(fd, spec) && term_core_ok(se->term);
        g_cps_mono_resolver = NULL;
        const Symbol *h0[1];
        island_mono = ok && is_cps_island(se->term, h0, 0);
        /* G3b: a NON-island monomorph (an escaping perform / colored callee) is
         * also DK-emittable when its generic is a surviving mono-template -- the
         * taint fixpoint proved its effect stays clean (all monomorphs admissible,
         * no fiber peer shares the effect).  Colored callers route to `__cps`. */
        mono_emit = ok && (island_mono || se->mono_template);
    }

    /* A spec clone of an in-S template that is NOT mono-emittable -- its
     * concrete signature is refused by mono_sig_ok (a heap handle result, say:
     * `(defn mx [A] [f : (fn [] A)] : A ...)` at `A := MxH`) -- is a SIG
     * rejection of that clone, and the clone keeps the direct path under its
     * own clone name, which is what its callers call.  Falling through here
     * rendered the TEMPLATE a second time under the BASE name: `redefinition of
     * mx__cps` / `mx`, found by tests/generic-spec-matrix.py (thunk/map/heap). */
    if (!mono_emit && spec && spec->fn == fd && spec->clone_name && se && se->in_s)
        return false;

    if (!mono_emit && (!se || !se->in_s)) {
        /* N6.5 gate: a COLORED function that falls back to the direct emitter must
         * be doing so for a PERMANENT signature reason (SIG-*) -- exported symbol,
         * program entry, ABI-incompatible signature, permanent taint, or an opaque
         * inline-C body the CPS backend can never own.  The fixable BODY-* eviction
         * surface has been driven to zero (docs/archive/cps-runtime-finish-plan.md),
         * so a BODY-* fallback now means a regression reintroduced a root the CPS
         * backend must own.  Categorize every colored fallback; a BODY-* one is a
         * HARD ERROR naming the residual, caught at build time instead of silently
         * routing to the (being-retired) fiber path.  The TUR_TRACE_EVICT trace
         * (readiness measurement) rides the same categorization. */
        if (fd->cps_colored && fd->binding) {
            const char *nm = fd->binding->name ? fd->binding->name->name : "?";
            /* `cat` is the permanent-vs-fixable classification: the SIG-* cases
             * are permanent routing (no BODY-* fix could admit them), the BODY-*
             * cases are fixable roots.  It fed a `sig_perm_route` flag read only
             * by the N6.5 hard error below; that error is unreachable (see
             * there), so the flag is gone and `cat` now feeds only the trace --
             * and would be what a restored diagnostic keys on. */
            const char *cat; const char *why = "";
            if (fd->binding->c_export_name
                && !fd->binding->is_instance_method)         { cat = "SIG-EXPORT"; }
            else if (fn_is_main(fd) && !fn_is_d2b_main(fd)) { cat = "SIG-MAIN";   }
            else if (!fn_sig_ok(fd))                        { cat = "SIG-REJECT"; }
            else {
                const CTerm *u = se ? first_unsupported(se->term) : NULL;
                /* SIG-TAINT: evicted ONLY because it shares an effect with a
                 * PERMANENT sig_perm fn (exported / main / ABI-reject) -- no
                 * BODY-* fix could admit it, so it is permanent routing, not a
                 * fixable BODY root.  A genuine BODY-UNSUPPORTED form still reports
                 * as such (the form is the real blocker even if also perm-tainted);
                 * only the taint-only (STRUCT-OR-TAINT) case reclassifies. */
                bool perm_tainted = se && ((se->eff_lo & g_perm_lo) || (se->eff_hi & g_perm_hi));
                bool inline_c = u && u->as.unsupported.why
                             && strstr(u->as.unsupported.why, "EX_INLINE_C");
                if (inline_c)          { cat = "SIG-INLINE-C"; }  /* permanent: inline-C can't thread a DK cont */
                else if (u) { cat = "BODY-UNSUPPORTED"; why = u->as.unsupported.why ? u->as.unsupported.why : "?"; }
                else if (perm_tainted) { cat = "SIG-TAINT"; }
                else {
                    /* Say which of the two it is: the structural core check
                     * (term_core_ok), the heap-join / escaping-join rules
                     * (needs_heap_join), or neither -- pure effect taint. */
                    bool core = se ? term_core_ok(se->term) : false;
                    bool hj = se ? needs_heap_join(se->term) : false;
                    cat = !core ? "BODY-STRUCT-CORE" : hj ? "BODY-STRUCT-JOIN" : "BODY-TAINT";
                }
            }
            if (getenv("TUR_TRACE_EVICT")) {
                /* Stage F (v2): an `eff=1` column marks a fn that performs or
                 * handles an effect -- the ONLY evictions that keep the fiber
                 * effect runtime alive.  A pure SIG-REJECT/SIG-EXPORT (eff=0) is
                 * out of scope for the deletion (plan Sec 4). */
                int eff = (se && (se->eff_lo || se->eff_hi)) ? 1 : 0;
                fprintf(stderr, "[EVICT] %-22s eff=%d %s %s\n", cat, eff, nm, why);
            }
            /* The N6.5 gate governs the SHIPPING backend.  The now-graduated CPS
             * surface -- `cps-async` (CPS-lowers `async`/`await`) and
             * `cps-tramp-resume` (trampolined effect tail-resume) -- deliberately
             * routes some bodies to the direct emitter by design: e.g. a recursive
             * `await` is a ready future the direct emitter handles in O(1) via its
             * inline readiness check + `goto __tur_tailcall` loop, so it evicts
             * rather than recurse through dk_invoke on the heap path.  (An `await`
             * inside a handler case instead delegates a fiber region without
             * evicting the whole function -- see b->in_handler_case in cps_ir.c.)
             * Exempt this surface from the hard error.
             *
             * THE N6.5 HARD ERROR IS CURRENTLY UNREACHABLE, and was already so
             * before this code said as much out loud.  The exemption read
             * `experimental_surface = g_opt_cps_tramp_resume`, and that bit has
             * been unwritable since cps-tramp-resume graduated (2026-07-19) --
             * always true, so `!sig_perm_route && !experimental_surface` never
             * held and the diagnostic could not fire.  It is deleted rather than
             * frozen as `if (... && false)`: a guard spelled with a literal is
             * the same dead branch with a worse disguise.
             *
             * To restore it, narrow the exemption to the shapes that actually
             * need it -- a recursive `await` (SIG-AWAIT-RECURSE) rather than the
             * whole CPS surface -- and re-emit the error for everything else.
             * The eviction is still visible meanwhile: TUR_TRACE_EVICT prints
             * every one, with its category and reason, from the block above. */
        }
        return false;   /* fall back (SIG-* colored, or uncolored) */
    }

    if (!g_fwd_done) { emit_forward_decls(ctx, file); g_fwd_done = true; }

    /* cps-self-tail-call-relies-on-sibling-call: is this function a member of
     * a mutual tail-call group?  Decided before the render, whose cell /
     * loop-carried scans the group check borrows. */
    int ctg_idx = -1;
    CpsTcg *grp = NULL;
    if (!mono_emit) {
        ctg_build(ctx);
        grp = ctg_of(fd, &ctg_idx);
    }
    char ctg_pfx[24];
    snprintf(ctg_pfx, sizeof ctg_pfx, "__tcgm%d_", ctg_idx);

    /* An island monomorph is emitted under its concrete clone name; the type
     * spellings + slot tiers in the reused generic CTerm resolve through the
     * active spec (g_cps_mono_resolver, set around the emit below). */
    char *cn = mono_emit ? strdup(spec->clone_name) : raw_name_for_binding(fd->binding);
    const Type *mono_ret = NULL;
    if (mono_emit) {
        g_cps_mono_resolver = ctx;
        mono_ret = &spec->result_type;
        /* Mirror emit_fns.c's by-value spec-param flagging: a concrete by-value
         * ADT-app param (`tur_adt_Option__int`) is passed by value, so a delegated
         * call *through* it (unwrap-or o ...) must NOT re-cross it as the int64
         * carrier.  The CPS emit_params does not set this, so set it here for each
         * such param -- otherwise the direct emitter reconstructs the aggregate
         * from a bogus `(intptr_t)(o)` carrier cast. */
        for (uint32_t i = 0; i < fd->n_params && i < spec->n_args; i++) {
            Type rat = emit_resolve_type(ctx, spec->arg_types[i]);
            const char *pc = emit_type_c_name(ctx, rat);
            if (fd->params[i] && pc && strcmp(pc, "int64_t") != 0
                && (type_uses_carrier_abi(rat) || type_app_is_concrete_adt(&rat)))
                fd->params[i]->emit_byvalue_carrier_abi = true;
        }
    }

    /* Set the function-parameter context so every reference to a parameter --
     * whether emitted by the CPS backend (name_for_binding) or by the direct
     * emitter inside a delegated CT_LETRAW body (emit_call_name / emit_value) --
     * resolves to the same raw (id-less) C name the direct emitter uses.  Without
     * this a fn-value parameter's declaration (id-suffixed) and its delegated use
     * (raw, e.g. `fnv.fn`) diverge; see
     * docs/archive/history/cps-backend-indirect-call-fatclosure-param-divergence.md.
     * Saved/restored around the whole emission (body + signature + wrapper). */
    Binding **saved_fn_params   = ctx->fn_params;
    uint8_t    saved_n_fn_params = ctx->n_fn_params;
    ctx->fn_params   = fd->params;
    ctx->n_fn_params = fd->n_params;

    /* inline-c-locals-invisible-to-inline-c-blocks: the same raw-spelling rule
     * for locals an inline-C block in this body names.  It has to be set on
     * BOTH emitters for the same reason the parameter context above does -- a
     * delegated CT_LETRAW body runs through the direct emitter, so a local
     * declared here and used from pasted C text must agree across the two. */
    const Binding **saved_ic_locals   = ctx->inline_c_raw_locals;
    uint32_t        saved_n_ic_locals = ctx->n_inline_c_raw_locals;
    emit_inline_c_raw_locals_collect(fd->body, fd->params, fd->n_params,
                                     &ctx->inline_c_raw_locals,
                                     &ctx->n_inline_c_raw_locals);

    /* Emit the body into a temporary buffer, accumulating any lifted reset/shift
     * helpers into `helpers`; then write helpers (which must precede their uses),
     * the signature, and the body. */
    Buf body_buf; buf_init(&body_buf);
    Buf helpers;  buf_init(&helpers);
    int helper_ctr = 0;
    CE ce; memset(&ce, 0, sizeof(ce));
    ce.ctx = ctx; ce.out = &body_buf; ce.helpers = &helpers; ce.indent = 4;
    ce.cur_k = "__kont"; ce.fn_cn = cn; ce.helper_ctr = &helper_ctr;
    ce.tb_bouncer = !fd->closure && fn_may_bounce(fd);
    ce.tb_out = &body_buf;
    ce.ret_ty = mono_ret ? mono_ret : fn_ret_type(fd);   /* KK_RET crossing type (Tier C aggregate return) */
    /* fn-value-fat-normalization (effect-row increment): a CAPTURING lambda's
     * __cps twin reads its captures exactly like the direct thunk does -- cast
     * the env param to the env struct and route capture reads through it.
     * atom_var / name_for_binding consult ctx->closure + ctx->env_var_name (the
     * same chokepoint the direct emitter's closure preamble uses), so setting
     * them around the body render is the whole integration. */
    struct Closure *saved_cps_closure = ctx->closure;
    const char *saved_cps_env_var = ctx->env_var_name;
    char *cps_env_var = NULL;
    if (fd->closure && fd->n_params > 0 && fd->closure->env_name) {
        /* The env struct must be DEFINED before this __cps body reads captures
         * through it -- the EX_CLOSURE construction site (which normally emits
         * it) lands later in the file.  Deduped via ctx->env_struct_names, so
         * whichever site runs first wins and the other skips. */
        emit_closure_env_struct_and_glue(ctx, file, fd->closure,
                                         fd->closure->env_name, false);
        ctx->closure = fd->closure;
        char *env_param_name = raw_name_for_binding(fd->params[0]);
        Buf ev; buf_init(&ev);
        buf_printf(&ev, "__env_%s", fd->closure->env_name->name);
        buf_putc(&ev, '\0');
        cps_env_var = strdup(ev.data);
        buf_free(&ev);
        buf_printf(&body_buf, "    struct %s *%s = (struct %s *)%s;\n",
                   fd->closure->env_name->name, cps_env_var,
                   fd->closure->env_name->name, env_param_name);
        free(env_param_name);
        ctx->env_var_name = cps_env_var;
    }
    /* B7: pre-scan this function's term for escaping-continuation mutables, which
     * emit as by-reference heap cells (see g_byref_muts). */
    g_byref_muts_n = 0;
    g_loop_carried_n = 0;
    /* Loop-carried params first: the promotion scan must know them before it
     * decides, since a loop can enclose the handle whose clause reads one. */
    loop_carried_scan(se->term);
    byref_scan(se->term);
    /* The term's own binders are `__t0` .. `__t<fresh_n-1>`, and the direct
     * emitter names its temporaries `__t<tmp_n>` from one program-wide
     * counter.  A monolithic build had emitted the stdlib before any user
     * function, so tmp_n was in the hundreds here; the prelude split's
     * program unit emits no stdlib, tmp_n starts at 0, and a delegated
     * node's `__t0` redeclared the term's. */
    if (ctx->tmp_n >= 0 && (uint32_t)ctx->tmp_n < se->fresh_n)
        ctx->tmp_n = (int)se->fresh_n;
    g_n_polyfn_cvars = 0;
    emit_binder_decls(&ce, se->term);
    /* cps-body-panic-not-propagated: every function this render produces -- the
     * `<fn>__cps` body, its join/frame/loop helpers -- returns the int64/intptr
     * word, so a delegated direct call's panic-signal check has a well-typed
     * early return here (`return 0` hands the C stack straight back to the
     * direct-entry wrapper, skipping `__kont`; the frames it abandons are
     * reap-owned).  With the ctype left NULL the check emitted only a comment,
     * and a CPS-colored function ran its remaining body -- and the whole
     * continuation -- after its callee (or its own `(panic ...)`) had panicked
     * under `catch-unwind`. */
    const char *saved_cps_ret_ctype = ctx->current_fn_ret_ctype;
    ctx->current_fn_ret_ctype = "int64_t";
    /* cps-self-tail-call-relies-on-sibling-call: a self tail call in this
     * body may jump back to here.  Not for a closure (its env is param 0), a
     * monomorph (the call names the generic), the program entry, or a param
     * the body keeps in a cell or as a loop-carried variable -- rebinding
     * the bare name would not reach those. */
    bool self_backedge = false;
    size_t body_start = body_buf.len;
    {
        bool ok = !mono_emit && !fd->closure && !fn_is_d2b_main(fd) && !fn_is_main(fd);
        for (uint32_t i = 0; ok && i < fd->n_params; i++)
            ok = fd->params[i] && !fd->params[i]->is_poly_fn &&
                 !is_byref_mut(fd->params[i]) && !is_loop_carried(fd->params[i]);
        /* A group member's self tail call is one of the group's jumps. */
        ce.self_fd = (ok && !grp) ? fd : NULL;
        ce.self_out = &body_buf;
        ce.self_backedge = &self_backedge;
        ce.tcg = grp;
        ce.tcg_idx = ctg_idx;
        ce.lbl_pfx = grp ? ctg_pfx : NULL;
    }
    emit_term(&ce, se->term);
    ctx->current_fn_ret_ctype = saved_cps_ret_ctype;
    if (self_backedge) {
        /* The label goes after the binder declarations, ahead of the first
         * statement of the body; the empty statement keeps it legal where a
         * declaration follows (C before C23). */
        static const char lbl[] = "__tur_cps_self:;\n";
        Buf withl; buf_init(&withl);
        buf_write(&withl, body_buf.data, body_start);
        buf_puts(&withl, lbl);
        buf_write(&withl, body_buf.data + body_start, body_buf.len - body_start);
        buf_free(&body_buf);
        body_buf = withl;
    }
    if (cps_env_var) {
        ctx->closure = saved_cps_closure;
        ctx->env_var_name = saved_cps_env_var;
        free(cps_env_var);
    }
    buf_putc(&body_buf, '\0');
    buf_putc(&helpers, '\0');

    /* A CT_LETRAW delegation to the direct emitter (e.g. a cloneable-reset)
     * emits its file-scope helper fns into ctx->pending_handler_fns, which the
     * direct-function path flushes ahead of the using function.  A CPS function
     * has its own emission path, so flush those helpers here too -- before the
     * __cps body that references them -- otherwise the helper is defined after
     * its use ('<helper>' undeclared).
     *
     * And before this function's own lifted HELPERS (its handler clauses, join
     * and continuation frames): a frame's body is emitted through the same
     * direct emitter, so a closure literal inside one -- the second of two
     * sequential `handle`s, whose body lands in the first's continuation --
     * puts its slot-0 widen wrapper here too, and the frame used it before its
     * definition ('__tur_widen___fn_N' undeclared).  The pending fns name only
     * file-scope functions the forward declarations already cover, never a
     * lifted CPS helper, so they can always go first. */
    if (ctx->pending_handler_fns && ctx->pending_handler_fns->len > 0) {
        buf_write(file, ctx->pending_handler_fns->data, ctx->pending_handler_fns->len);
        buf_free(ctx->pending_handler_fns);
        buf_init(ctx->pending_handler_fns);
    }

    if (helpers.len > 1) buf_puts(file, helpers.data);

    if (grp) {
        /* cps-self-tail-call-relies-on-sibling-call: the body is this
         * member's case of the group's fused function; `<name>__cps` is the
         * way in. */
        Buf cb; buf_init(&cb);
        for (uint32_t k = 0; k < fd->n_params; k++) {
            char *pn = name_for_binding(ctx, fd->params[k]);
            buf_printf(&cb, "        %s %s = __tcg_s%u;\n        (void)%s;\n",
                       grp->slot_ctype[grp->slot_base[ctg_idx] + k], pn,
                       (unsigned)(grp->slot_base[ctg_idx] + k), pn);
            free(pn);
        }
        buf_puts(&cb, body_buf.data);
        buf_puts(&cb, "        return 0;\n");
        buf_putc(&cb, '\0');
        free(grp->body[ctg_idx]);
        grp->body[ctg_idx] = strdup(cb.data);
        buf_free(&cb);
        if (!grp->declared) {
            ctg_emit_signature(file, grp, false);
            buf_puts(file, ";\n");
            grp->declared = true;
        }
        buf_printf(file, "static int64_t %s__cps(", cn);
        emit_params(ctx, file, fd);
        if (fd->n_params) buf_puts(file, ", ");
        buf_printf(file, "DK *__kont) {\n    return %s(%d", grp->name, ctg_idx);
        for (int m = 0; m < grp->n_mem; m++)
            for (uint32_t k = 0; k < grp->mem[m]->n_params; k++) {
                if (m == ctg_idx) {
                    char *pn = name_for_binding(ctx, fd->params[k]);
                    buf_printf(file, ", %s", pn);
                    free(pn);
                } else {
                    buf_printf(file, ", (%s){0}", grp->slot_ctype[grp->slot_base[m] + k]);
                }
            }
        buf_puts(file, ", __kont);\n}\n");
        bool all = true;
        for (int m = 0; m < grp->n_mem; m++) if (!grp->body[m]) all = false;
        if (all) ctg_emit_def(file, grp);
    } else {
    /* ---- CPS body: int64_t <name>__cps(<params>, DK *__kont) ---- */
    buf_printf(file, "%sint64_t %s__cps(",
               (emit_split_lib_owns(fd->binding) && !mono_emit) ? "" : "static ", cn);
    emit_params(ctx, file, fd);
    if (fd->n_params) buf_puts(file, ", ");
    buf_puts(file, "DK *__kont) {\n");
    buf_puts(file, body_buf.data);
    buf_puts(file, "}\n");
    }
    buf_free(&body_buf);
    buf_free(&helpers);

    /* ---- D2b fixed-ABI entry wrapper: int main(int argc, char **argv) ----
     * A delimited zero-arg `main` (cps-backend-direct-lowering-removal-plan D2b)
     * keeps the program's fixed entry ABI: the wrapper reproduces the direct
     * emitter's `main` prologue (panic-trace flag + the *args* cons build from
     * argv, mirroring emit_fns.c / emit_module.c), seeds the root prompt exactly
     * like the generic wrapper below, trampolines into `main__cps`, and returns
     * the delivered value as the process exit code. */
    if (fn_is_d2b_main(fd)) {
        const Type *mrt = mono_ret ? mono_ret : fn_ret_type(fd);
        bool mvoid = (mrt->kind == TY_NIL);
        buf_puts(file, "int main(int argc, char **argv) {\n");
        emit_main_deep_stack_prologue(file);
        buf_puts(file, "    __tur_static_init();\n");   /* S1b */
        emit_win_binary_stdio_prologue(file);
        if (g_emit_panic_trace)
            buf_puts(file, "    g_panic_trace = 1;\n");
        buf_puts(file, "    /* *args*: build cons list from argv[1..argc-1]; *argv0* is argv[0] */\n");
        buf_puts(file, "    if (argc > 0 && argv[0]) g_tur_argv0 = argv[0];\n");
        buf_puts(file, "    g_tur_args = 0;\n");
        buf_puts(file, "    for (int _ai = argc - 1; _ai >= 1; _ai--) {\n");
        buf_puts(file, "        typedef struct { int64_t value; int64_t next; } __tur_args_cell;\n");
        buf_puts(file, "        __tur_args_cell *_c = (__tur_args_cell *)malloc(sizeof(__tur_args_cell));\n");
        buf_puts(file, "        _c->value = (int64_t)(intptr_t)argv[_ai];\n");
        buf_puts(file, "        _c->next = g_tur_args;\n");
        buf_puts(file, "        g_tur_args = (int64_t)(intptr_t)_c;\n");
        buf_puts(file, "    }\n");
        buf_puts(file, "    __dk_entry_depth++;\n");
    /* r7rs-callcc-memory-never-freed: where this entry's registrations
     * start, so a nested exit can drop them (__dk_reap_drop_to). */
    buf_puts(file, "    size_t __dk_reap_mark = __dk_reap_n;\n");
        buf_puts(file, "    DK *__root = dk_prompt(DK_ROOT_TAG, dk_done());\n");
        /* E7: install the trampoline driver.  A tail-resume longjmps here; the
         * else-branch runs the meta-stack trampoline to completion. */
        if (!mvoid) buf_puts(file, "    int64_t __r;\n");
        buf_puts(file, "    tur_jmp_buf __dkjb; tur_jmp_buf *__dksave = g_dk_driver; g_dk_driver = &__dkjb;\n");
        buf_printf(file, "    if (TUR_SETJMP(__dkjb) == 0) { %s%s__cps(__root); }\n",
                   mvoid ? "(void)" : "__r = ", cn);
        buf_printf(file, "    else { %s__dk_drive_after(); }\n", mvoid ? "(void)" : "__r = ");
        buf_puts(file, "    g_dk_driver = __dksave;\n");
        /* Read the delivered value out BEFORE the reap: a Tier-C return rides a
         * heap box owned by the reap list (consume=false, never freed at a load),
         * so the reap frees it -- copy the value into a local first, then reap. */
        if (!mvoid) {
            char *ld = slot_load(ctx, mrt->kind, mrt, "__r", false);
            buf_printf(file, "    int __mret = (int)(%s);\n", ld);
            free(ld);
        }
        /* cps-async (F3.2/gap-2): if the body PARKED on a pending await, a
         * lifted continuation copy may still reference __root (a RESET_CONT
         * await frame carries k=__root in its env); leak it rather than dangle.
         * tur_async_suspended is always 0 for a synchronous / effect-only body,
         * so this is byte-identical there.  The reap list is gated the same way:
         * a parked continuation may still reference a registered chain / env
         * struct / box, so reaping only runs once the body has actually settled
         * (docs/archive/cps-delimited-dk-node-leak.md). */
        buf_puts(file, "    if (!tur_async_suspended) dk_free(__root);\n");
        buf_puts(file, "    if (!tur_async_suspended) { if (--__dk_entry_depth == 0) __dk_reap_run(); else __dk_reap_drop_to(__dk_reap_mark); }\n");
        if (mvoid) {
            buf_puts(file, "    return 0;\n}\n");
        } else {
            buf_puts(file, "    return __mret;\n}\n");
        }
        free((void *)ctx->inline_c_raw_locals);
        ctx->inline_c_raw_locals   = saved_ic_locals;
        ctx->n_inline_c_raw_locals = saved_n_ic_locals;
        ctx->fn_params   = saved_fn_params;
        ctx->n_fn_params = saved_n_fn_params;
        g_cps_mono_resolver = NULL;
        free(cn);
        return true;
    }

    /* ---- direct->cps entry wrapper: int64_t <name>(<params>) ----
     * The boundary trampoline for an uncolored/direct caller (including `main`)
     * entering a colored function by its plain name unchanged.  It seeds the
     * initial continuation as the implicit root prompt (CPS5.3, DK_ROOT_TAG) on
     * a dk_done()-terminated chain -- the structural equivalent of dk_run_root,
     * so an undelimited capture inside the CPS body reaches program entry -- runs
     * the CPS body, and returns (unwraps) the value delivered to the root.  The
     * seed chain is freed after the body returns; the CPS body never retains it
     * (a captured sub-continuation is always a copy). */
    const Type *rt = mono_ret ? mono_ret : fn_ret_type(fd);
    /* A nil/void return: the entry wrapper is declared `void` (matching the
     * direct emitter's forward decl) and discards the unit the CPS body delivers,
     * rather than returning an int64. */
    bool void_ret = (rt->kind == TY_NIL);
    const char *rety = void_ret ? "void" : binder_ctype_full(ctx, rt->kind, rt);
    /* Linkage must mirror the direct emitter's `needs_static` (emit_fns.c) and
     * the forward decl (emit_fn_forward_decls): in separate compilation an
     * exported (or retain_c_linkage) non-stdlib defn keeps external linkage so
     * the plain-mangled entry wrapper is reachable from its forward declaration
     * and from other TUs / the FFI export shim.  A bare `static` here on an
     * exported CPS defn contradicts its non-static prototype -- "static
     * declaration follows non-static declaration". */
    /* definstance-not-dispatchable-across-modules: an instance method is never
     * `is_exported`, but a user module's instance now keeps external linkage so
     * an importing TU can dispatch on it -- mirror that here, or this path
     * emits the very contradiction the comment above warns about. */
    bool entry_static = !(ctx->separate_compilation
        && fd->binding
        && (fd->binding->is_exported || fd->binding->retain_c_linkage
            || emit_inst_method_wants_external(fd))
        && !fd->binding->is_from_stdlib)
        && !(emit_split_lib_owns(fd->binding) && !mono_emit);   /* r7rs-programs-compile-slowly */
    buf_printf(file, "__attribute__((unused)) %s%s %s(",
               entry_static ? "static " : "", rety, cn);
    /* Params: the __cps ABI spelling (emit_params) EXCEPT a pass-by-ptr
     * aggregate, which is spelled `const T *` to agree with the direct forward
     * decl (the wrapper is the plain-name symbol direct callers link against). */
    for (uint32_t i = 0; i < fd->n_params; i++) {
        if (i) buf_puts(file, ", ");
        char *pn = name_for_binding(ctx, fd->params[i]);
        if (cps_entry_param_by_ptr(e, fd, i)) {
            buf_printf(file, "const %s *%s",
                       cps_entry_param_byptr_ctype(e, fd, i), pn);
        } else {
            buf_printf(file, "%s %s", emit_param_ctype(ctx, fd, i), pn);
        }
        free(pn);
    }
    buf_puts(file, ") {\n");
    /* proper-tail-calls T6 (T-D6): a bouncer's direct entry is where the
     * trampoline's arming lands -- the driver arms exactly this function, and
     * the fat box's shim calls it directly.  Armed, it publishes its root as
     * `tur_tb_root`, which is how a tail dynamic call in the `__cps` body knows
     * its continuation is this entry's and may bounce back to the driver.
     * Saved and restored around the body so a nested entry cannot leak its
     * root outward. */
    bool tb_entry = !fd->closure && fn_may_bounce(fd);
    /* r7rs-conformance-program-emits-megabytes-of-c: a zero-parameter entry
     * that is not a bouncer is the shared __dk_enter0 helper (emit_module.c,
     * beside tur_async_suspended) -- the same sequence as below, once per
     * program instead of once per function.  The result is read the way the
     * inline wrapper reads it: a boxed (Tier-C) value is copied out by the
     * helper before its reap, a word converted after (a pure conversion). */
    if (fd->n_params == 0 && !tb_entry) {
        if (void_ret) {
            buf_printf(file, "    (void)__dk_enter0(%s__cps, NULL, 0);\n    return;\n}\n", cn);
        } else {
            Type _rr; const Type *rrt = cps_resolve_ty(rt, &_rr);
            if (slot_box_ty(rrt)) {
                buf_printf(file, "    %s __ret;\n"
                                 "    (void)__dk_enter0(%s__cps, &__ret, sizeof __ret);\n"
                                 "    return __ret;\n}\n", rety, cn);
            } else {
                char *ld = slot_load(ctx, rt->kind, rt, "__r", false);
                buf_printf(file, "    int64_t __r = __dk_enter0(%s__cps, NULL, 0);\n"
                                 "    return %s;\n}\n", cn, ld);
                free(ld);
            }
        }
        goto entry_wrapper_done;
    }
    buf_puts(file, "    __dk_entry_depth++;\n");
    /* r7rs-callcc-memory-never-freed: where this entry's registrations
     * start, so a nested exit can drop them (__dk_reap_drop_to). */
    buf_puts(file, "    size_t __dk_reap_mark = __dk_reap_n;\n");
    buf_puts(file, "    DK *__root = dk_prompt(DK_ROOT_TAG, dk_done());\n");
    if (tb_entry) {
        ensure_saffron_dyn_runtime(ctx);
        buf_puts(file, "    void *__tb_save = tur_tb_root; tur_tb_root = NULL;\n");
        buf_printf(file, "    if (tur_tb_armed_for == (void *)%s) { tur_tb_armed_for = NULL; "
                         "tur_tb_root = (void *)__root; }\n", cn);
    }
    /* Build the argument list "<params>, __root" once.  A const-pointer param
     * (cps_entry_param_by_ptr) is dereferenced back to the by-value `__cps`
     * signature. */
    Buf __args; buf_init(&__args);
    for (uint32_t i = 0; i < fd->n_params; i++) {
        if (i) buf_puts(&__args, ", ");
        char *pn = name_for_binding(ctx, fd->params[i]);
        if (cps_entry_param_by_ptr(e, fd, i)) buf_putc(&__args, '*');
        buf_puts(&__args, pn);
        free(pn);
    }
    if (fd->n_params) buf_puts(&__args, ", ");
    buf_puts(&__args, "__root");
    buf_putc(&__args, '\0');
    /* E7: install the trampoline driver at this direct->cps entry too (not only
     * the d2b main), so a colored body reached from a non-d2b caller (`run`
     * called by a plain main) still trampolines a deep tail-resume flat. */
    if (!void_ret) buf_puts(file, "    int64_t __r;\n");
    buf_puts(file, "    tur_jmp_buf __dkjb; tur_jmp_buf *__dksave = g_dk_driver; g_dk_driver = &__dkjb;\n");
    buf_printf(file, "    if (TUR_SETJMP(__dkjb) == 0) { %s%s__cps(%s); }\n",
               void_ret ? "(void)" : "__r = ", cn, __args.data);
    /* T6: after a tail-resume longjmp the drive runs continuations that are not
     * this entry's body; no bounce shortcut applies there. */
    buf_printf(file, "    else { %s%s__dk_drive_after(); }\n",
               tb_entry ? "tur_tb_root = NULL; " : "", void_ret ? "(void)" : "__r = ");
    buf_puts(file, "    g_dk_driver = __dksave;\n");
    if (tb_entry) buf_puts(file, "    tur_tb_root = __tb_save;\n");
    buf_free(&__args);
    /* Read the delivered value out BEFORE the reap: a Tier-C return rides a heap
     * box owned by the reap list (consume=false, never freed at a load), so the
     * reap frees it -- copy the value into a local first, then reap. */
    if (!void_ret) {
        char *ld = slot_load(ctx, rt->kind, rt, "__r", false);
        /* A boxed (Tier-C) return is read through the box pointer, and a body
         * that panicked returned before delivering one: its `if
         * (tur_panicking) return 0;` check leaves __r NULL.  Reading through
         * it was the SIGSEGV under `catch-unwind` in
         * saffron-catch-unwind-around-dyn-call-fn-crashes -- the panic never
         * reached the boundary that was waiting to catch it.  A zero value
         * goes back instead, and the caller's own panic check propagates. */
        Type _rr; const Type *rrt = cps_resolve_ty(rt, &_rr);
        if (slot_box_ty(rrt))
            buf_printf(file, "    %s __ret = __r ? %s : (%s){0};\n", rety, ld, rety);
        else
            buf_printf(file, "    %s __ret = %s;\n", rety, ld);
        free(ld);
    }
    /* cps-async (F3.2/gap-2): if the body PARKED on a pending await, a
     * lifted continuation copy may still reference __root (a RESET_CONT
     * await frame carries k=__root in its env); leak it rather than dangle.
     * tur_async_suspended is always 0 for a synchronous / effect-only body,
     * so this is byte-identical there.  The reap list is gated the same way:
     * a parked continuation may still reference a registered chain / env
     * struct / box, so reaping only runs once the body has actually settled
     * (docs/archive/cps-delimited-dk-node-leak.md). */
    buf_puts(file, "    if (!tur_async_suspended) dk_free(__root);\n");
    buf_puts(file, "    if (!tur_async_suspended) { if (--__dk_entry_depth == 0) __dk_reap_run(); else __dk_reap_drop_to(__dk_reap_mark); }\n");
    if (void_ret) {
        buf_puts(file, "    return;\n}\n");
    } else {
        buf_puts(file, "    return __ret;\n}\n");
    }
entry_wrapper_done:

    /* E2a: a threadable captureless effectful lambda registers its direct-entry ->
     * __cps mapping at startup, so a threaded call site recovers its CPS variant. */
    if (threadable_has(fd->binding)) {
        /* fnsan-e2a-registry-typed-entry: the E2a call site casts a looked-up
         * entry to `int64_t (*)([void *,] W0.., DK *)` with every Wi the word
         * except a float kind, which travels at its own type (e2a_cast).  An
         * entry declared with a POINTER (or narrow) parameter --
         * `__fn_7__cps(const char *msg, DK *)` -- was called through that
         * cast: an indirect call through the wrong function type (a
         * -fsanitize=function trap, a call_indirect trap on WASM).  Register
         * an adapter in the call site's convention instead. */
        bool e2_adapt = false, e2_ok = true;
        const char *e2pc[MAX_FN_ARITY];
        uint32_t e2n = fd->n_params;
        if (e2n > MAX_FN_ARITY) e2_ok = false;
        for (uint32_t i = 0; e2_ok && i < e2n; i++) {
            const char *pc = emit_param_ctype(ctx, fd, i);
            e2pc[i] = pc;
            if (!pc) { e2_ok = false; break; }
            size_t pL = strlen(pc);
            bool env = fd->closure && i == 0;
            bool flt = strcmp(pc, "double") == 0 || strcmp(pc, "float") == 0;
            bool word = strcmp(pc, "int64_t") == 0;
            bool ptr = pL >= 1 && pc[pL - 1] == '*';
            bool narrow = strcmp(pc, "bool") == 0 || strcmp(pc, "int8_t") == 0 ||
                          strcmp(pc, "int16_t") == 0 || strcmp(pc, "int32_t") == 0 ||
                          strcmp(pc, "uint8_t") == 0 || strcmp(pc, "uint16_t") == 0 ||
                          strcmp(pc, "uint32_t") == 0;
            if (env || flt || word) continue;
            if (ptr || narrow) { e2_adapt = true; continue; }
            e2_ok = false;              /* an aggregate: keep the direct entry */
        }
        if (e2_ok && e2_adapt) {
            buf_printf(file, "static int64_t %s__e2w(", cn);
            for (uint32_t i = 0; i < e2n; i++) {
                const char *pc = e2pc[i];
                bool keep = (fd->closure && i == 0) || strcmp(pc, "double") == 0 ||
                            strcmp(pc, "float") == 0 || strcmp(pc, "int64_t") == 0;
                buf_printf(file, "%s a%u, ", keep ? pc : "int64_t", i);
            }
            buf_printf(file, "DK *__k) {\n    return %s__cps(", cn);
            for (uint32_t i = 0; i < e2n; i++) {
                const char *pc = e2pc[i];
                size_t pL = strlen(pc);
                bool keep = (fd->closure && i == 0) || strcmp(pc, "double") == 0 ||
                            strcmp(pc, "float") == 0 || strcmp(pc, "int64_t") == 0;
                if (keep)
                    buf_printf(file, "a%u, ", i);
                else if (pc[pL - 1] == '*')
                    buf_printf(file, "(%s)(intptr_t)a%u, ", pc, i);
                else
                    buf_printf(file, "(%s)a%u, ", pc, i);
            }
            buf_puts(file, "__k);\n}\n");
        }
        buf_printf(file,
            "static void __tur_e2reg_%s(void) {\n"
            "    __tur_cps_register((intptr_t)%s, (__tur_cps_fn)%s%s);\n"
            "}\n", cn, cn, cn, (e2_ok && e2_adapt) ? "__e2w" : "__cps");
        /* S1b: the direct->CPS registry must be populated before a threaded
         * call site looks up its CPS variant.  A dropped constructor here was
         * the SIGSEGV in findings 3.1. */
        char e2init[320];
        snprintf(e2init, sizeof(e2init), "__tur_e2reg_%s", cn);
        static_init_register(e2init, STATIC_INIT_REGISTRY);
    }

    free((void *)ctx->inline_c_raw_locals);
    ctx->inline_c_raw_locals   = saved_ic_locals;
    ctx->n_inline_c_raw_locals = saved_n_ic_locals;
    ctx->fn_params   = saved_fn_params;
    ctx->n_fn_params = saved_n_fn_params;
    g_cps_mono_resolver = NULL;   /* G3a: end of island-monomorph emit (no-op otherwise) */
    free(cn);
    return true;
}
