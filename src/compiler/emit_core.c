/* emit_core.c -- shared codegen helpers: naming, atoms, builtins, captures. */
#include "emit_internal.h"
#include "mangle.h"
#include "platform_fs.h"  /* strndup() on Windows */
#include "globals.h"      /* compiler config globals */
#include "cps.h"          /* cps_visit_children (ownership provenance walk) */

/* ------------ helpers ------------ */

/* Gap E: count items a single top-level Expr contributes after flattening.
 * EX_DEFMODULE spreads its body; an EX_DO at top level recursively spreads
 * each of its children (so a macro can emit `(do (defn ...) (defn ...))`
 * and the children land as ordinary top-level items). Other forms count
 * as one. */
static uint32_t flatten_count(const Expr *e) {
    if (!e) return 0;
    if (e->kind == EX_DEFMODULE) {
        DefModule *mod = e->as.defmodule_.mod;
        uint32_t n = 0;
        for (uint32_t j = 0; j < mod->n_body; j++) n += flatten_count(mod->body[j]);
        return n;
    }
    if (e->kind == EX_DO) {
        uint32_t n = 0;
        for (uint32_t j = 0; j < e->as.do_.n; j++) n += flatten_count(e->as.do_.items[j]);
        return n;
    }
    return 1;
}

/* Gap E counterpart of flatten_count: write each post-flatten item into
 * `flat[*k]` and bump `*k`. */
static void flatten_emit(const Expr *e, const Expr **flat, uint32_t *k) {
    if (!e) return;
    if (e->kind == EX_DEFMODULE) {
        DefModule *mod = e->as.defmodule_.mod;
        for (uint32_t j = 0; j < mod->n_body; j++) flatten_emit(mod->body[j], flat, k);
        return;
    }
    if (e->kind == EX_DO) {
        for (uint32_t j = 0; j < e->as.do_.n; j++) flatten_emit(e->as.do_.items[j], flat, k);
        return;
    }
    flat[(*k)++] = e;
}

/* Phase M0: Flatten EX_PROGRAM items into a contiguous array, expanding any
 * EX_DEFMODULE nodes into their body items.
 *
 * Gap E (2026-06-11): also flatten top-level `(do ...)` forms recursively,
 * matching Common Lisp's top-level-progn semantics. This lets a macro emit
 * a single `(do (defn name-impl ...) (def name (make-system ... name-impl)))`
 * expansion and have both top-level forms land in the items array as
 * siblings. Without it, the do flows through emit_stmt() and aborts with
 * "EX_FN_DEF in stmt position" the moment a defn appears inside. See
 * docs/archive/history/macro-cannot-emit-multiple-top-level-forms.md.
 *
 * The returned array is malloc'd and must be freed by the caller. */
const Expr **flatten_program_items(const Expr *program, uint32_t *out_n) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        total += flatten_count(program->as.program.items[i]);
    }
    const Expr **flat = (const Expr **)malloc(total * sizeof(Expr *));
    if (!flat && total > 0) { fprintf(stderr, "tur: oom\n"); abort(); }
    uint32_t k = 0;
    for (uint32_t i = 0; i < program->as.program.n; i++) {
        flatten_emit(program->as.program.items[i], flat, &k);
    }
    *out_n = total;
    return flat;
}

/* Helper to create a Type from TypeKind (mirrors the one in types.c). */
Type emit_type_from_kind(TypeKind k) {
    Type t = {0};
    t.kind = k;
    t.as.fn.arity = 0;
    t.hkt_kind = KIND_STAR;  /* Phase HKT-P6: all types are kind * in v1 */
    return t;
}

/* ASan/LSan plan (Option C): allocate transient Type scratch from the emit
 * pass's arena when one is available, falling back to malloc otherwise. The
 * arena is bulk-freed at the end of emit_program / emit_implementation, so
 * these nodes are no longer leaked (they are reachable via the arena until
 * the compile finishes, which is exactly their lifetime). */
static void *emit_type_scratch(EmitCtx *ctx, size_t size) {
    if (ctx && ctx->type_arena) return arena_alloc(ctx->type_arena, size);
    void *p = malloc(size);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    return p;
}

static bool emit_find_abi_binding(const EmitAbiSpecialization *spec,
                                  const char *name, uint8_t *out_idx) {
    if (!spec || !name) return false;
    for (uint8_t i = 0; i < spec->n_bindings; i++) {
        if (spec->bindings[i].name && strcmp(spec->bindings[i].name, name) == 0) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

/* R3 (carrier-crossing-recovery-routing-plan): does this type's structural spine
 * still carry an unresolved parametric param (a TY_TYVAR)?  A recovered concrete
 * type that answers `true` is the exact signature of a carrier<->concrete crossing
 * whose monomorphization was not fully recovered -- the silent-miscompile defect
 * the routing chokepoints exist to prevent.  Consulted by the R3 debug gate AND by
 * emit_reresolve_disp_type's Edge-1 bail (both Debug and Release), so it is not
 * Debug-only. */
static bool type_spine_has_tyvar(const Type *t, int depth) {
    if (!t || depth > 8) return false;
    if (t->kind == TY_TYVAR) return true;
    if (t->kind == TY_APP)
        return type_spine_has_tyvar(t->as.app.fn, depth + 1) ||
               type_spine_has_tyvar(t->as.app.arg, depth + 1);
    return false;
}

/* R3 chokepoint gate: assert that a type recovered by a carrier<->concrete
 * recovery chokepoint is concrete *enough* before it flows into code emission.
 * A leftover parametric param means the crossing was mis-routed -- the value or
 * dispatch would silently fall back to the int64 carrier where a concrete
 * representation was required, surfacing later as a downstream miscompile.  This
 * flips that into an immediate, local `tur` ICE.
 *
 * `deep` selects the strictness, which differs by side:
 *   - false (value side): a recovered value type may be a TY_APP container whose
 *     element legitimately rides the carrier (e.g. `(Option A)` inside an
 *     option_map spec), so only a *bare* TY_TYVAR -- a param type that is wholly
 *     unresolved -- is a routing hole.
 *   - true (dispatch side): the recovered type SELECTS a concrete `__inst_*`, so
 *     any tyvar anywhere in its spine would mis-select; the whole spine must be
 *     tyvar-free.
 *
 * Debug-only (compiled out under NDEBUG / Release).  The `TUR_ABI_NO_ROUTE_ICE`
 * environment escape hatch downgrades the ICE to a one-line warning, so an
 * in-flight migration that knowingly trips the invariant can still produce
 * output while it is being fixed. */
void emit_abi_assert_routed_concrete(EmitCtx *ctx, const Type *recovered,
                                     const char *site, bool deep) {
#ifndef NDEBUG
    if (!recovered) return;
    bool unrouted = deep ? type_spine_has_tyvar(recovered, 0)
                         : (recovered->kind == TY_TYVAR);
    if (!unrouted) return;
    const EmitAbiSpecialization *spec = ctx ? ctx->current_abi_specialization : NULL;
    const char *spec_name = (spec && spec->clone_name) ? spec->clone_name : "?";
    if (getenv("TUR_ABI_NO_ROUTE_ICE")) {
        fprintf(stderr, "tur: warning: carrier<->concrete crossing not routed to "
                "a concrete type at %s (spec %s, type kind %d); downgraded by "
                "TUR_ABI_NO_ROUTE_ICE\n", site, spec_name, (int)recovered->kind);
        return;
    }
    fprintf(stderr,
            "tur: internal error (ICE): carrier<->concrete crossing reached code "
            "emission with an unresolved parametric param at %s.\n"
            "  active spec : %s\n"
            "  type kind   : %d (a recovery chokepoint returned a non-concrete "
            "type)\n"
            "This is a 'forgot to route' routing hole "
            "(docs/archive/history/carrier-crossing-recovery-routing-plan.md, R3).\n"
            "Set TUR_ABI_NO_ROUTE_ICE=1 to downgrade to a warning while fixing.\n",
            site, spec_name, (int)recovered->kind);
    abort();
#else
    (void)ctx; (void)recovered; (void)site; (void)deep;
#endif
}

/* RM3 R5 graduation item 3: return the region pool at exit.
 *
 * A reclaimed generation is REWOUND, not released -- arena_reset keeps the
 * slabs and region.c pools the arena for the next push -- so at exit the pool
 * (one 64 KiB slab per pooled arena) was reachable but never freed, and every
 * leak checker reported it.  tur_region_shutdown frees live, retired and
 * pooled arenas outright.
 *
 * WHERE it is registered is the whole problem, and the first attempt got it
 * wrong in a way the plan predicted ("care about atexit ordering against
 * module defers that may still read retired generations").  atexit runs
 * handlers LIFO, so shutdown must be REGISTERED BEFORE every module `defer`
 * (a defer may read a value that escaped its bracket, which lives in a
 * RETIRED generation; freeing first is a use-after-free, and the probe read 0
 * where it should read 42, with valgrind's "Invalid read").  The defers are
 * registered by __tur_static_init's STATIC_INIT_ATEXIT band -- and
 * __tur_static_init is ALSO run from a __attribute__((constructor)) wrapper
 * BEFORE main.  So registering shutdown in any main prologue is too late: the
 * constructor has already registered the defers.  The only place that is
 * always earlier is the first statement of __tur_static_init itself, behind
 * its idempotent guard: it runs exactly once, from whichever path enters
 * first, and before any band.  That is the single call site, in
 * static_init_emit.
 *
 * Gated on the flag so a flag-off program references no region symbol and the
 * linker never extracts the member (R2's reasoning).  A panic aborts and skips
 * atexit, which is fine -- abort is abnormal exit and frees nothing anyway. */
void emit_region_shutdown_atexit(Buf *out, int indent) {
    if (!regions_enabled()) return;
    buf_printf(out, "%*satexit(tur_region_shutdown);\n", indent, "");
}

/* region-lock-hardening (see emit_internal.h).  The classification is by the
 * C spelling because that is what decides whether `(intptr_t)lv` is a
 * reinterpretation (a word) or a numeric conversion (a double -- undefined
 * for out-of-range values, and never region memory anyway).  Anything that is
 * neither a known scalar nor a word is treated as an aggregate and noted by
 * its bytes, which is sound for every shape: a word that is not a pointer
 * compares as an address and is not region memory. */
void emit_region_note_lvalue(Buf *body, int indent, const char *ctype, const char *lv) {
    if (!regions_enabled() || !lv) return;
    if (ctype) {
        static const char *skip[] = {
            "double", "float", "bool", "_Bool", "char", "short", "int", "long",
            "unsigned", "int8_t", "int16_t", "int32_t", "uint8_t", "uint16_t",
            "uint32_t", "uint64_t", "size_t", "void", NULL };
        for (int i = 0; skip[i]; i++)
            if (strcmp(ctype, skip[i]) == 0) return;
    }
    /* Always the WORDS form.  It is valid C for any lvalue -- an int64
     * carrier, a pointer, a by-value aggregate, a 16-byte poly-fn -- and the
     * claimed `ctype` is not always the emitted one: a carrier-spelled ADT
     * can be a by-value aggregate under the active spec, and `(intptr_t)` of
     * an aggregate is a hard cc error ("aggregate value used where an integer
     * was expected"), found the first time this was emitted as
     * TUR_REGION_NOTE(lv).  One word costs the same either way. */
    buf_printf(body, "%*sTUR_REGION_NOTE_WORDS(&(%s), sizeof(%s));\n", indent, "", lv, lv);
}

Type emit_resolve_type(EmitCtx *ctx, Type t) {
    const EmitAbiSpecialization *spec = ctx ? ctx->current_abi_specialization : NULL;
    if (!spec) return t;
    switch (t.kind) {
        case TY_TYVAR: {
            uint8_t idx = 0;
            if (t.as.tyvar_.name && emit_find_abi_binding(spec, t.as.tyvar_.name, &idx)) {
                return spec->bindings[idx].type;
            }
            return t;
        }
        case TY_APP: {
            if (!t.as.app.fn || !t.as.app.arg) return t;
            Type fn = emit_resolve_type(ctx, *t.as.app.fn);
            Type arg = emit_resolve_type(ctx, *t.as.app.arg);
            /* constrained-hkt-abstract-var-requires-last-param-free: mirror the
             * two instantiation paths -- when the head resolved to a hole-headed
             * partial application, saturate through the hole so a spec body's
             * `(m a)` materialises as `Result__int__cstr`, matching the
             * signature emit_abi_instantiate_type already mangled. */
            if (type_app_has_hole(&fn)) {
                uint8_t hp = type_app_hole_pos(&fn);
                Type ctor  = fn.as.app.fn ? *fn.as.app.fn : fn;
                Type fixed = fn.as.app.arg ? *fn.as.app.arg : arg;
                Type first  = (hp == 0) ? arg   : fixed;
                Type second = (hp == 0) ? fixed : arg;
                Type inner;
                memset(&inner, 0, sizeof(inner));
                inner.kind = TY_APP;
                inner.copy_kind = ctor.copy_kind;
                inner.as.app.fn  = (Type *)emit_type_scratch(ctx, sizeof(Type));
                inner.as.app.arg = (Type *)emit_type_scratch(ctx, sizeof(Type));
                *inner.as.app.fn  = ctor;
                *inner.as.app.arg = first;
                Type outer;
                memset(&outer, 0, sizeof(outer));
                outer.kind = TY_APP;
                outer.copy_kind = ctor.copy_kind;
                outer.as.app.fn  = (Type *)emit_type_scratch(ctx, sizeof(Type));
                outer.as.app.arg = (Type *)emit_type_scratch(ctx, sizeof(Type));
                *outer.as.app.fn  = inner;
                *outer.as.app.arg = second;
                return outer;
            }
            Type out = t;
            out.as.app.fn = (Type *)emit_type_scratch(ctx, sizeof(Type));
            out.as.app.arg = (Type *)emit_type_scratch(ctx, sizeof(Type));
            *out.as.app.fn = fn;
            *out.as.app.arg = arg;
            return out;
        }
        case TY_FN: {
            if (!t.as.fn.arg_full_types && !t.as.fn.result_full_type) return t;
            Type out = t;
            if (t.as.fn.arity > 0 && t.as.fn.arg_full_types && t.as.fn.arg_kinds) {
                Type **nfull = (Type **)emit_type_scratch(
                    ctx, (size_t)t.as.fn.arity * sizeof(Type *));
                uint8_t *nkinds = (uint8_t *)emit_type_scratch(
                    ctx, (size_t)t.as.fn.arity * sizeof(uint8_t));
                for (uint32_t i = 0; i < t.as.fn.arity; i++) {
                    nkinds[i] = t.as.fn.arg_kinds[i];
                    nfull[i] = t.as.fn.arg_full_types[i];
                    if (!t.as.fn.arg_full_types[i]) continue;
                    Type sub = emit_resolve_type(ctx, *t.as.fn.arg_full_types[i]);
                    Type *slot = (Type *)emit_type_scratch(ctx, sizeof(Type));
                    *slot = sub;
                    /* Only adopt the substitution when it actually RESOLVED.
                     * A context whose bindings do not cover this tyvar leaves
                     * `sub` a TY_TYVAR; overwriting the stable erased kind with
                     * it renames the monomorph (`fn1_int__int` ->
                     * `fn1_struct__struct`, `struct` being the tyvar token) so
                     * the definition no longer matches its forward declaration. */
                    if (sub.kind == TY_TYVAR || sub.kind == TY_UNKNOWN) continue;
                    nfull[i] = slot;
                    nkinds[i] = (uint8_t)sub.kind;
                }
                out.as.fn.arg_full_types = nfull;
                out.as.fn.arg_kinds = nkinds;
            }
            if (t.as.fn.result_full_type) {
                Type sub = emit_resolve_type(ctx, *t.as.fn.result_full_type);
                Type *slot = (Type *)emit_type_scratch(ctx, sizeof(Type));
                *slot = sub;
                if (sub.kind != TY_TYVAR && sub.kind != TY_UNKNOWN) {
                    out.as.fn.result_full_type = slot;
                    out.as.fn.result_kind = sub.kind;
                }
            }
            return out;
        }
        case TY_UNION: {
            Type out = t;
            if (t.as.union_.n_members == 0 || !t.as.union_.members) return out;
            Type **members = (Type **)emit_type_scratch(ctx, t.as.union_.n_members * sizeof(Type *));
            out.as.union_.members = members;
            for (uint8_t i = 0; i < t.as.union_.n_members; i++) {
                members[i] = (Type *)emit_type_scratch(ctx, sizeof(Type));
                *members[i] = emit_resolve_type(ctx, *t.as.union_.members[i]);
            }
            return out;
        }
        case TY_INTERSECTION: {
            Type out = t;
            if (t.as.intersection_.n_members == 0 || !t.as.intersection_.members) return out;
            Type **members = (Type **)emit_type_scratch(ctx, t.as.intersection_.n_members * sizeof(Type *));
            out.as.intersection_.members = members;
            for (uint8_t i = 0; i < t.as.intersection_.n_members; i++) {
                members[i] = (Type *)emit_type_scratch(ctx, sizeof(Type));
                *members[i] = emit_resolve_type(ctx, *t.as.intersection_.members[i]);
            }
            return out;
        }
        default:
            return t;
    }
}

const char *emit_type_c_name(EmitCtx *ctx, Type t) {
    return type_c_name(emit_resolve_type(ctx, t));
}

/* fn-typed-return: does this body statically evaluate to a *thin* (bare,
 * non-capturing) function pointer?
 *
 * A non-capturing fn literal is lifted to a top-level function and the body
 * becomes an EX_VAR referencing that global (non-boxed TY_FN) binding -- a
 * bare C function pointer value.  By contrast a capturing EX_CLOSURE is a fat
 * box, and a call/local-binding result is not statically known to be thin (it
 * may be a fat box typed as a plain fn, e.g. the value a capturing-closure
 * constructor returns).  Only the thin case may be typed as a fn pointer; the
 * rest stay on the existing int64_t/void* carrier.  do/let/if/ascribe wrappers
 * are transparent; an `if` is thin only when both arms are thin. */
static bool body_yields_thin_fn(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_FN:
            return true;
        case EX_VAR:
            return e->as.var.binding && e->as.var.binding->is_global &&
                   e->as.var.binding->type.kind == TY_FN &&
                   !e->as.var.binding->type.as.fn.boxed;
        case EX_REINTERPRET:
            return body_yields_thin_fn(e->as.reinterpret_.expr);
        case EX_ASCRIBE:
            return body_yields_thin_fn(e->as.ascribe_.inner);
        case EX_DO:
            for (int i = (int)e->as.do_.n - 1; i >= 0; i--)
                if (e->as.do_.items[i]->kind != EX_DEFER)
                    return body_yields_thin_fn(e->as.do_.items[i]);
            return false;
        case EX_LET:
        case EX_LETREC:
            return body_yields_thin_fn(e->as.let_.body);
        case EX_IF:
            return e->as.if_.else_or_null &&
                   body_yields_thin_fn(e->as.if_.then_) &&
                   body_yields_thin_fn(e->as.if_.else_or_null);
        default:
            return false;
    }
}

/* fn-typed-return: a `defn` whose declared return type is a concrete,
 * non-boxed function type returns a *function value* (a bare/thin function
 * pointer), not a value of the function's result type.  type_c_name(TY_FN)
 * lowers a non-boxed primitive-result fn to its result type's C name (the
 * "bare function reference returns its result" convention used by inline-C
 * function values), which is wrong for a producer that returns the closure:
 * the C signature would say `double` while the body returns `double (*)(double)`.
 *
 * Return the matching fn-ptr typedef name (e.g. tur_fnptr_double_double_t) so
 * the signature, forward declaration, and the thin-fn-pointer let binding at
 * the consumer all agree.  Returns NULL when the return type is not a concrete
 * thin function value (boxed closures, dict-dispatched method impls, bodies
 * that do not statically yield a thin fn pointer, non-primitive arg/result
 * kinds, and non-fn returns all fall back to the existing lowering). */
const char *emit_fn_return_typedef(const FnDef *fd, const Type *rft) {
    if (!fd || !rft || rft->kind != TY_FN) return NULL;
    /* A boxed first-class closure is the void* carrier, not a thin fn ptr. */
    if (rft->as.fn.boxed) return NULL;
    /* A typeclass-method impl is reached through its dictionary.  The dict
     * field type and the impl signature must agree and both must match what
     * the body produces.  A thin (non-capturing) body yields a bare fn
     * pointer, so it uses the same fn-ptr typedef as a plain defn here; the
     * dict field is updated in lockstep (see emit_inst_fn_return_carrier and
     * the dict-struct emission in emit_stmt.c).  A capturing __inst_ method
     * is a fat box and is steered onto the int64_t carrier by
     * emit_inst_fn_return_carrier instead (body_yields_thin_fn is false for
     * it, so this helper returns NULL below). */
    /* Only a body that statically yields a thin (non-capturing) function
     * pointer may be typed as one.  Capturing closures and other fn-typed
     * values stay on the existing int64_t/void* carrier so the box is not
     * mistyped as a bare function pointer. */
    if (!body_yields_thin_fn(fd->body)) return NULL;
    /* register_fn_ptr_typedef returns NULL unless every arg/result kind is a
     * concrete primitive -- exactly the thin-fn-pointer case. */
    return register_fn_ptr_typedef(rft);
}

/* instance-method-closure-return: the carrier for a typeclass-method impl
 * (`__inst_*`) whose declared return type is a concrete (non-boxed) function
 * value.  The dictionary field type and the impl signature both consult this,
 * so they agree with each other AND with the body the impl emits:
 *
 *   - thin (non-capturing) body  -> the matching fn-ptr typedef (a bare C
 *     function pointer is returned, so the slot must be that pointer type);
 *   - everything else (capturing fat box, etc.) -> the int64_t closure
 *     carrier (a void* heap box returned through an int64_t slot).
 *
 * type_c_name(TY_FN) would instead pick the function's *result* type's C name
 * ("double" for a (fn [...] :float) return) -- correct for inline-C raw
 * function references, but wrong for a turmeric-level closure value: the impl
 * body returns a function pointer / box, not a value of the result type, so
 * for non-int result kinds the emitted C is a hard `cc` error.  Returns NULL
 * for non-`__inst_` functions and non-fn returns (use the normal lowering). */
const char *emit_inst_fn_return_carrier(const FnDef *fd, const Type *rft) {
    if (!fd || !rft || rft->kind != TY_FN || rft->as.fn.boxed) return NULL;
    if (!(fd->binding && fd->binding->name && fd->binding->name->name &&
          strncmp(fd->binding->name->name, "__inst_", 7) == 0))
        return NULL;
    const char *td = emit_fn_return_typedef(fd, rft);
    return td ? td : "int64_t";
}

/* KB-021: the single arbiter of which types may use the int64_t carrier ABI
 * (a heap-pointer handle) for dictionary-dispatched typeclass methods.
 *
 * Carrier-ABI types:
 *   - TY_APP            (e.g. (Vec int), (Box int)) -- parametric container apps
 *   - TY_ADT            (algebraic data types, already int64_t in type_c_name)
 *   - parametric struct (TY_STRUCT with n_type_params > 0)
 *
 * These types have two coexisting C value representations: the int64_t carrier
 * (returned by carrier-ABI stdlib functions like (vec-new)/(some x)) and a
 * by-value concrete struct (a struct constructor literal, or an ABI-specialized
 * clone that returns the concrete type).  The dictionary-dispatch callsites and
 * the let-binding declaration path both consult this predicate (together with
 * the per-expression representation check) so they agree about whether a value
 * is already a carrier or a by-value aggregate that must be bridged. */
bool type_uses_carrier_abi(Type t) {
    /* SC7: a transparent int newtype has a single int64 representation -- it is
     * not a carrier-ABI aggregate, so no spill/box/deref bridging applies. */
    if (type_is_transparent_int_newtype(t)) return false;
    /* CONV-S1: a non-parametric flat-product ADT flows by value -- it is a
     * concrete aggregate, NOT a carrier, so it spills/boxes/derefs through the
     * same emit_carrier_bridge machinery a by-value struct uses.  Gated by
     * adt_is_byvalue_product (LIVE for leaf products as of B3). */
    if (t.kind == TY_ADT && adt_is_byvalue_product(t.as.adt_.def)) return false;
    /* seam 3: a non-parametric :heap ADT is a concrete typed pointer
     * `tur_adt_<Name> *` (not the int64 carrier), exactly as a non-parametric
     * :heap struct is not carrier-ABI -- so no spill/box/deref bridging. */
    if (t.kind == TY_ADT && t.as.adt_.def && t.as.adt_.def->is_heap &&
        t.as.adt_.def->n_type_params == 0) return false;
    /* seam 3: a :heap ADT app (lowered Vec/Map/Set/...) is a typed POINTER whose
     * bit pattern IS the int64 carrier -- it DOES use the carrier ABI (its
     * inline-C bases take/return int64), exactly as a parametric :heap STRUCT app
     * does.  Keep it on the carrier path (fall through to the TY_APP return below)
     * rather than letting the by-value-product check misclassify it as a by-value
     * aggregate (which would declare a let binding as `tur_adt_Vec__int *` and
     * spill+address-of at carrier sinks). */
    if (t.kind == TY_APP && adt_app_is_byvalue_product(t) &&
        !type_is_heap_adt(t)) return false;
    /* structdef-retirement slice 5: an opaque newtype is a plain int64 carrier,
     * NOT a carrier-ABI aggregate -- so no spill/box/deref bridging applies (a
     * bare `defopaque` value is already the int64).  Mirrors the old opaque-
     * STRUCT rule below: carrier-ABI only when it carries phantom type params
     * (a parametric opaque application rides the int64 handle like any TY_APP). */
    if (t.kind == TY_ADT && t.as.adt_.def && t.as.adt_.def->is_opaque)
        return t.as.adt_.def->n_type_params > 0 &&
               !adt_opaque_c_names_as_pointer(t.as.adt_.def);
    /* opaque-pointer-c-spelling: a PARAMETRIC opaque over a pointer
     * (`(Goal int)`, `(Parser cstr)`, `(SChan P)`) is a `void *` handle in
     * exactly the sense a bare one is -- its phantom arguments are erased and
     * the pointer IS the value.  Left on the carrier path it would be DECLARED
     * `int64_t` at let-bindings while type_c_name spells it `void *`, which is
     * the seam the repr shadow ICEs on (`(Goal int)`: want=scalar-bits
     * got=carrier-i64 cty=int64_t own=void *). */
    if (t.kind == TY_APP && adt_opaque_c_names_as_pointer(type_adt_app_def(&t)))
        return false;
    if (t.kind == TY_APP || t.kind == TY_ADT) return true;
    return false;
}

bool emit_fn_body_is_opaque_ptr_over_carrier_result(const FnDef *fd,
                                                    TypeKind declared_result) {
    if (!fd || !fd->body) return false;
    Type bt = fd->body->type;
    const AdtDef *def = bt.kind == TY_ADT ? bt.as.adt_.def
                      : bt.kind == TY_APP ? type_adt_app_def(&bt)
                                          : NULL;
    if (!adt_opaque_c_names_as_pointer(def)) return false;
    const char *dc = type_c_name(emit_type_from_kind(declared_result));
    return dc && strcmp(dc, "int64_t") == 0;
}

/* structdef-retirement DS-C: emit_carrier_return_override (RT/SC5) is deleted.
 * Its signal was a by-value (non-carrier) TY_STRUCT method body type -- which no
 * longer occurs, since every struct lowers to a record ADT.  The function always
 * returned the "none" sentinel, so all five call sites' `carrier_override.kind
 * == TY_STRUCT` branches were dead and have been removed. */

void indent_buf(Buf *b, int n) {
    for (int i = 0; i < n; i++) buf_putc(b, ' ');
}

/* Debugger Phase 4 (--debug): see emit_internal.h. */
void emit_line_reset(EmitCtx *ctx) {
    ctx->dbg_last_line = 0;
    ctx->dbg_last_file_id = 0;
}

void emit_line_directive(EmitCtx *ctx, Buf *out, Span span) {
    if (!g_emit_debug_lines) return;
    if (span.line == 0) return;                 /* SPAN_UNKNOWN / synthetic node */
    const char *path = diag_file_path(span.file_id);
    if (!path) return;                          /* file_id not registered */
    if (ctx->dbg_last_line == span.line &&
        ctx->dbg_last_file_id == span.file_id)
        return;                                 /* same source line as previous */
    /* A `#` preprocessor directive must begin a line.  Most emit sites leave
     * the stream at column 0 (statements end in "\n"), but guard against a
     * mid-line position so the directive never lands inside a statement. */
    if (out->len > 0 && out->data[out->len - 1] != '\n')
        buf_putc(out, '\n');
    /* Escape backslashes and double quotes so a path with either stays a valid
     * C string literal (Windows-style separators, unusual filenames). */
    buf_puts(out, "#line ");
    char num[16];
    snprintf(num, sizeof(num), "%u", span.line);
    buf_puts(out, num);
    buf_puts(out, " \"");
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '"') buf_putc(out, '\\');
        buf_putc(out, *p);
    }
    buf_puts(out, "\"\n");
    ctx->dbg_last_line = span.line;
    ctx->dbg_last_file_id = span.file_id;
}

/* Phase R1: Helper to check if an expression is fully divergent (never
 * produces a value).  Used by emit_let_value to decide whether to assign
 * the body's emitted value to the let's result tmp: a divergent body
 * emits no value to assign. */
bool expr_is_divergent(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_RETURN:
        case EX_PANIC:
        case EX_PANIC_WITH:
        case EX_DISCONTINUE:
            return true;
        case EX_DO:
            if (e->as.do_.n == 0) return false;
            /* find last non-defer item */
            for (int i = (int)e->as.do_.n - 1; i >= 0; i--) {
                if (e->as.do_.items[i]->kind != EX_DEFER) {
                    return expr_is_divergent(e->as.do_.items[i]);
                }
            }
            return false;
        case EX_LET:
            return expr_is_divergent(e->as.let_.body);
        case EX_IF:
            if (!e->as.if_.else_or_null) return false;
            return expr_is_divergent(e->as.if_.then_) &&
                   expr_is_divergent(e->as.if_.else_or_null);
        default:
            return false;
    }
}

/* Phase 3/4: Helper to check if an expression contains return or panic */
bool expr_contains_return_or_throw(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_RETURN:
        case EX_PANIC:
        case EX_PANIC_WITH:
            return true;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (expr_contains_return_or_throw(e->as.do_.items[i])) {
                    return true;
                }
            }
            return false;
        case EX_LET:
            /* Check body for return/throw */
            return expr_contains_return_or_throw(e->as.let_.body);
        case EX_IF:
            /* Check both branches */
            if (expr_contains_return_or_throw(e->as.if_.then_)) return true;
            if (e->as.if_.else_or_null) {
                return expr_contains_return_or_throw(e->as.if_.else_or_null);
            }
            return false;
        case EX_WHILE:
            /* Check body */
            return expr_contains_return_or_throw(e->as.while_.body);
        /* Phase 19: Algebraic effects - discontinue is like throw */
        case EX_DISCONTINUE:
            return true;
        default:
            return false;
    }
}

/* closure-drop-glue S1c: does `e`'s subtree contain any inline-C block?  A
 * `^fat`/fn-typed parameter referenced inside an inline-C body can be STORED by
 * the C text (e.g. `s[2] = (int64_t)f;` in schema/transform), which the
 * AST-level closure escape analysis cannot see -- a param is a C-visible formal,
 * not an AST capture.  So a body containing ANY inline-C must NOT be treated as
 * non-retaining.  Conservative by construction: an unmodeled node kind returns
 * true (assume it might hide inline-C), so the non-retention inference only ever
 * DISqualifies a fn -- it never wrongly greenlights a free.  Only the common,
 * fully-understood control/leaf kinds return false. */
/* ---------------------------------------------------------------------------
 * inline-c-locals-invisible-to-inline-c-blocks
 *
 * A function PARAMETER reaches an inline-C block by its source name -- that is
 * what the `ctx->fn_params` branch of name_for_binding buys, and what
 * raw_name_for_binding's own comment describes ("inline-C bodies reference
 * them by their source names").  A `let`-bound LOCAL did not: it takes the
 * `<name>_<id>` path, so
 *
 *     (let [key (rvec-get raw i)]
 *       ```c vec->data[j + 1] = key; ```)
 *
 * emitted `'key' undeclared` from the C compiler, deep in generated code, with
 * nothing pointing back at the Turmeric line.
 *
 * The id suffix exists so two bindings with the same source name cannot
 * collide in C, so it cannot simply be dropped.  Instead a local is spelled
 * raw only when doing so is unambiguous AND actually needed:
 *
 *   - the name is already a plain C identifier (no kebab, no sigils), so the
 *     legacy mangler is the identity on it;
 *   - it is neither a C keyword nor a libc symbol;
 *   - no other local in the same function body, and no parameter, shares it
 *     (no shadowing to resolve);
 *   - it carries no storage indirection of its own (atomic / thread-local /
 *     by-ref cell / extern-c / c_export_name), which have their own spellings;
 *   - and one of the function's inline-C blocks mentions it as an identifier
 *     token.
 *
 * That last condition is what keeps this from churning codegen: a function
 * whose inline C never names a local emits byte-for-byte what it emitted
 * before.
 * ------------------------------------------------------------------------- */

static bool ic_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

/* True when `name` occurs in `ic`'s C text as a whole identifier token.  This
 * over-approximates on purpose -- a hit inside a string literal or a comment
 * costs at most a raw spelling for a name that was already unambiguous. */
static bool ic_text_names(const InlineC *ic, const char *name, size_t len) {
    if (!ic || !ic->code.p || len == 0) return false;
    const char *p = ic->code.p;
    size_t n = ic->code.len;
    if (len > n) return false;
    for (size_t i = 0; i + len <= n; i++) {
        if (memcmp(p + i, name, len) != 0) continue;
        if (i > 0 && ic_ident_char(p[i - 1])) continue;
        if (i + len < n && ic_ident_char(p[i + len])) continue;
        return true;
    }
    return false;
}

typedef struct {
    const Binding **locals; uint32_t n_locals; uint32_t cap_locals;
    const InlineC **ics;    uint32_t n_ics;    uint32_t cap_ics;
} ICScan;

static void ic_scan_push_local(ICScan *sc, const Binding *b) {
    if (!b) return;
    if (sc->n_locals == sc->cap_locals) {
        sc->cap_locals = sc->cap_locals ? sc->cap_locals * 2 : 16;
        sc->locals = (const Binding **)realloc(sc->locals,
                                               sc->cap_locals * sizeof(*sc->locals));
        if (!sc->locals) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sc->locals[sc->n_locals++] = b;
}

static void ic_scan_push_ic(ICScan *sc, const InlineC *ic) {
    if (!ic) return;
    if (sc->n_ics == sc->cap_ics) {
        sc->cap_ics = sc->cap_ics ? sc->cap_ics * 2 : 8;
        sc->ics = (const InlineC **)realloc(sc->ics, sc->cap_ics * sizeof(*sc->ics));
        if (!sc->ics) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sc->ics[sc->n_ics++] = ic;
}

/* Walk the kinds that can hold a `let` or an inline-C block.  An unmodeled kind
 * simply contributes nothing, which degrades to today's behaviour (the local
 * keeps its id suffix) rather than to a wrong name. */
static void ic_scan_expr(ICScan *sc, const Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EX_INLINE_C:
            ic_scan_push_ic(sc, e->as.inline_c_.inline_c);
            return;
        case EX_FN_DEF:
            /* Emitted as its own C function; it runs its own collection. */
            return;
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                ic_scan_push_local(sc, e->as.let_.bindings[i].binding);
                ic_scan_expr(sc, e->as.let_.bindings[i].init);
            }
            ic_scan_expr(sc, e->as.let_.body);
            return;
        case EX_CALL:
            ic_scan_expr(sc, e->as.call_.fn_expr);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                ic_scan_expr(sc, e->as.call_.args[i]);
            ic_scan_expr(sc, e->as.call_.dict_arg);
            return;
        case EX_IF:
            ic_scan_expr(sc, e->as.if_.cond);
            ic_scan_expr(sc, e->as.if_.then_);
            ic_scan_expr(sc, e->as.if_.else_or_null);
            return;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) ic_scan_expr(sc, e->as.do_.items[i]);
            return;
        case EX_WHILE:
            ic_scan_expr(sc, e->as.while_.cond);
            ic_scan_expr(sc, e->as.while_.body);
            return;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++) ic_scan_expr(sc, e->as.builtin.args[i]);
            return;
        case EX_MATCH:
            ic_scan_expr(sc, e->as.match_.scrutinee);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                const MatchArm *arm = &e->as.match_.arms[i];
                for (uint32_t j = 0; j < arm->pattern.n_bindings; j++)
                    ic_scan_push_local(sc, arm->pattern.bindings[j]);
                ic_scan_push_local(sc, arm->pattern.var_binding);
                ic_scan_expr(sc, arm->guard);
                ic_scan_expr(sc, arm->body);
            }
            return;
        case EX_REINTERPRET: ic_scan_expr(sc, e->as.reinterpret_.expr); return;
        case EX_ASCRIBE: ic_scan_expr(sc, e->as.ascribe_.inner); return;
        case EX_CAST:    ic_scan_expr(sc, e->as.cast_.expr);     return;
        case EX_RETURN:  ic_scan_expr(sc, e->as.return_.value);  return;
        default:
            return;
    }
}

static bool ic_name_is_plain_c_ident(const char *p, size_t len) {
    if (len == 0) return false;
    if (!((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z') || p[0] == '_'))
        return false;
    for (size_t i = 1; i < len; i++) if (!ic_ident_char(p[i])) return false;
    return true;
}

void emit_inline_c_raw_locals_collect(const Expr *body,
                                      Binding **params, uint32_t n_params,
                                      const Binding ***out, uint32_t *out_n) {
    *out = NULL;
    *out_n = 0;
    if (!body) return;

    ICScan sc;
    memset(&sc, 0, sizeof sc);
    ic_scan_expr(&sc, body);
    if (sc.n_ics == 0 || sc.n_locals == 0) { free(sc.locals); free(sc.ics); return; }

    const Binding **keep = (const Binding **)malloc(sc.n_locals * sizeof(*keep));
    if (!keep) { fprintf(stderr, "tur: oom\n"); abort(); }
    uint32_t n_keep = 0;

    for (uint32_t i = 0; i < sc.n_locals; i++) {
        const Binding *b = sc.locals[i];
        if (!b || !b->name || b->is_global || b->is_extern_c || b->c_export_name) continue;
        if (b->is_atomic || b->is_thread_local) continue;
        if (emit_binding_is_byref_cell(b)) continue;
        const char *nm = b->name->name;
        size_t nl = b->name->len;
        if (!ic_name_is_plain_c_ident(nm, nl)) continue;
        if (tur_name_is_c_keyword(nm, nl) || tur_name_collides_libc(nm, nl)) continue;

        /* Unshadowed: no other collected local, and no parameter, shares the
         * source name.  With two `key`s in one function there is no single raw
         * spelling that could mean either, so both keep their id suffix. */
        bool ambiguous = false;
        for (uint32_t j = 0; j < sc.n_locals && !ambiguous; j++) {
            if (j == i) continue;
            const Binding *o = sc.locals[j];
            if (o && o != b && o->name && o->name->len == nl
                && memcmp(o->name->name, nm, nl) == 0) ambiguous = true;
        }
        for (uint32_t j = 0; j < n_params && !ambiguous; j++) {
            const Binding *pb = params ? params[j] : NULL;
            if (pb && pb->name && pb->name->len == nl
                && memcmp(pb->name->name, nm, nl) == 0) ambiguous = true;
        }
        if (ambiguous) continue;

        /* Needed: some inline-C block in this function names it. */
        bool named = false;
        for (uint32_t j = 0; j < sc.n_ics && !named; j++)
            named = ic_text_names(sc.ics[j], nm, nl);
        if (!named) continue;

        keep[n_keep++] = b;
    }

    free(sc.locals);
    free(sc.ics);
    if (n_keep == 0) { free(keep); return; }
    *out = keep;
    *out_n = n_keep;
}

bool expr_subtree_has_inline_c(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_INLINE_C:
            return true;
        /* leaves: no inline-C */
        case EX_NIL_LIT: case EX_BOOL_LIT: case EX_INT_LIT: case EX_FLOAT_LIT:
        case EX_CSTR_LIT: case EX_SYM_LIT: case EX_VAR: case EX_DICT:
        case EX_FN_DEF:   /* a nested fn-def's own body is analyzed on its own */
            return false;
        case EX_CALL:
            if (expr_subtree_has_inline_c(e->as.call_.fn_expr)) return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (expr_subtree_has_inline_c(e->as.call_.args[i])) return true;
            return expr_subtree_has_inline_c(e->as.call_.dict_arg);
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (expr_subtree_has_inline_c(e->as.let_.bindings[i].init)) return true;
            return expr_subtree_has_inline_c(e->as.let_.body);
        case EX_IF:
            return expr_subtree_has_inline_c(e->as.if_.cond)
                || expr_subtree_has_inline_c(e->as.if_.then_)
                || expr_subtree_has_inline_c(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (expr_subtree_has_inline_c(e->as.do_.items[i])) return true;
            return false;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (expr_subtree_has_inline_c(e->as.builtin.args[i])) return true;
            return false;
        case EX_MATCH:
            if (expr_subtree_has_inline_c(e->as.match_.scrutinee)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (expr_subtree_has_inline_c(e->as.match_.arms[i].guard)) return true;
                if (expr_subtree_has_inline_c(e->as.match_.arms[i].body)) return true;
            }
            return false;
        case EX_ASCRIBE: return expr_subtree_has_inline_c(e->as.ascribe_.inner);
        case EX_CAST:    return expr_subtree_has_inline_c(e->as.cast_.expr);
        /* any-struct-box-leak-per-widen: the `any` readers carry no inline-C of
         * their own -- each lowers to emitter-generated tag arithmetic or a
         * deref -- so only their operand needs walking.  Left to the
         * conservative `default` they reported "may hide inline-C" for every
         * body that so much as looks at an `any`, which silently switched off
         * the whole nonretain inference below (this one and the catch-box
         * confinement that has always shared it) for those bodies. */
        case EX_ANY_TYPE_OF: return expr_subtree_has_inline_c(e->as.any_type_of_.value);
        case EX_ANY_IS:      return expr_subtree_has_inline_c(e->as.any_is_.value);
        case EX_ANY_CAST:    return expr_subtree_has_inline_c(e->as.any_cast_.value);
        /* A field READ is likewise inline-C-free in itself (a field WRITE is a
         * different node and keeps the conservative default), and it is common
         * enough that leaving it unmodelled switched the inference off for most
         * bodies that touch a struct at all.  The `default` below stays
         * conservative for everything still unlisted. */
        case EX_GET_FIELD:   return expr_subtree_has_inline_c(e->as.get_field_.struct_expr);
        /* saffron-lang-plan S5: the three dynamic nodes, for exactly the reason
         * the `any` readers above were added.  Each lowers to emitter-generated
         * code -- a preamble helper call, a fat-protocol dispatch, a tag chain --
         * and carries no inline-C of its own, so only the operands need walking.
         *
         * Left to the conservative `default` they answered "may hide inline-C"
         * for every Saffron body, which switched the whole nonretain inference
         * off before its result gate was even consulted.  Measured, not
         * reasoned: `(defn get-x [p] (.x p))` reported inlinec=1 under a probe
         * on the inference's entry, which is why its `any` argument was
         * heap-boxed at every call site.  A dynamic CALL is walked like an
         * ordinary one -- the question is whether THIS body contains inline-C,
         * not what the callee does. */
        case EX_DYN_OP:
            for (uint32_t i = 0; i < e->as.dyn_op_.n_args; i++)
                if (expr_subtree_has_inline_c(e->as.dyn_op_.args[i])) return true;
            return false;
        case EX_DYN_CALL:
            if (expr_subtree_has_inline_c(e->as.dyn_call_.fn)) return true;
            for (uint32_t i = 0; i < e->as.dyn_call_.n_args; i++)
                if (expr_subtree_has_inline_c(e->as.dyn_call_.args[i])) return true;
            return false;
        case EX_DYN_FIELD:   return expr_subtree_has_inline_c(e->as.dyn_field_.obj);
        case EX_RETURN:  return expr_subtree_has_inline_c(e->as.return_.value);
        /* RM1 (bind chains): a nested closure literal is walked INTO -- its
         * captures are copies of this body's values, so inline C in there can
         * stash them just as inline C here could -- and the packings around a
         * function value / a carrier reinterpret are walked through.  Left to
         * `default`, a body holding any lambda (every hoisted `bind`
         * continuation) read as "may hide inline C" and lost every inference
         * that keys on this predicate. */
        case EX_CLOSURE:
            return e->as.closure_.closure && e->as.closure_.closure->fn &&
                   expr_subtree_has_inline_c(e->as.closure_.closure->fn->body);
        case EX_POLY_WRAP:   return expr_subtree_has_inline_c(e->as.poly_wrap_.inner);
        case EX_FN_TO_FAT:   return expr_subtree_has_inline_c(e->as.fn_to_fat_.inner);
        case EX_POLY_TO_FAT: return expr_subtree_has_inline_c(e->as.poly_to_fat_.inner);
        case EX_REINTERPRET: return expr_subtree_has_inline_c(e->as.reinterpret_.expr);
        /* any-widen-stored-in-an-adt-field-has-no-owner: a WIDEN is a packing
         * around its operand, exactly like the four above, and hides nothing --
         * the emitter's own code for it is a malloc-and-copy or a frame-box, no
         * user C anywhere.  Left to `default` it read as "may hide inline C",
         * which made elab_infer_nonretain_masks skip the WHOLE body of every
         * function whose result is a widen.  In Saffron that is most functions,
         * because an unannotated return IS `any` -- so the frame-box rule that
         * saffron-any-return-defeats-the-frame-box-rule landed never ran for
         * them, and each call widening a by-value payload malloc'd a box
         * nothing freed.
         *
         * That archived report fixed this same failure one node kind over
         * (EX_DYN_OP / EX_DYN_CALL / EX_DYN_FIELD had no arm either) and its
         * lesson was that the entry gate, not the result gate, was the first
         * blocker.  EX_UNION_INJECT was the arm it did not add.  The coloring
         * walk had the identical hole (cps-coloring-walk-has-no-arm-for-union-inject),
         * so this node has now been the missing arm in three walks. */
        case EX_UNION_INJECT:
            return expr_subtree_has_inline_c(e->as.union_inject_.value);
        /* byvalue-recursive-shared-copies-leak: an rc count operation and a
         * scope-exit `defer` are emitter code around their operand.  The rc
         * field auto-drop IS an injected defer, and the clone of a shared view
         * an rc clone, so leaving these to `default` switched the inference off
         * for every body holding an rc-field struct local -- the same missing
         * arm this function has grown three times before. */
        case EX_RC_CLONE: return expr_subtree_has_inline_c(e->as.rc_clone_.expr);
        case EX_RC_DROP:  return expr_subtree_has_inline_c(e->as.rc_drop_.expr);
        case EX_DEFER:    return expr_subtree_has_inline_c(e->as.defer_.body);
        default:
            /* Unmodeled kind -- conservatively assume it may hide inline-C. */
            return true;
    }
}

/* Fat-closure-env scoped-free escape analysis
 * (docs/archive/history/fat-closure-env-leak.md).
 *
 * Returns true if the binding `b` -- which holds a freshly-constructed fat
 * closure value (the heap env `malloc`'d by the EX_CLOSURE emitter) -- is used
 * anywhere in `e` as a *value* (passed as an argument, stored into a field/var,
 * captured by a nested closure, returned, borrowed, ...) rather than solely as
 * the callee of a direct call `(b ...)`.  When this returns false the closure
 * provably does not escape its defining scope, so its env may be `free`d at
 * scope exit (see emit_let_value).
 *
 * Soundness posture: the analysis only ever GREENLIGHTS a free, so it must
 * never miss an escape.  The `default` arm therefore reports an escape (true)
 * for any Expr kind not explicitly understood -- a new or rare node kind that
 * could hide a reference to `b` conservatively disables the optimization rather
 * than risking a use-after-free.  A reference to `b` inside a nested closure is
 * detected via that closure's precomputed capture set (EX_CLOSURE/EX_FN_DEF),
 * so the "callee position is fine" relaxation never leaks into a nested body.
 * EX_DEFER runs at the same scope-exit point as the free, but can only reach
 * `b` through its capture set, so it is an escape only when it actually captures
 * `b` (mirroring EX_CLOSURE); a defer that does not reference the closure -- e.g.
 * an owning sibling binding's injected auto-drop `(defer (drop r))` -- does not
 * block the free. */
/* catch-unwind-return-bridge-residuals (Part A): true for a scalar `err-val`
 * result type -- an integer/float/bool value the extraction copies out by value.
 * For a caught box, err-val hands back box->err_val, which IS the panic-payload
 * pointer; when the declared err arm is one of these scalar kinds the extracted
 * value is a plain word (a truncated/reinterpreted pointer bit pattern), NEVER a
 * live pointer INTO the payload, so freeing the box (payload included) at scope
 * exit cannot dangle it.  A cstr/ptr/aggregate err arm WOULD alias the payload,
 * so it stays excluded (the status-quo leak).  Broader than carrier_is_inline:
 * that predicate excludes the pointer-sized int/int64/uint64 (no reinterpret
 * needed), which are exactly the common err arms here and are equally safe. */
static bool err_val_result_is_freeable_scalar(TypeKind k) {
    switch (k) {
        case TY_INT: case TY_INT64: case TY_UINT64:
        case TY_INT32: case TY_UINT32: case TY_INT16: case TY_UINT16:
        case TY_INT8: case TY_UINT8:
        case TY_BOOL:
        case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32:
            return true;
        default:
            return false;
    }
}

/* catch-unwind-returned-err-box-payload-leak: true when `t` is an
 * already-resolved `(Result A B)` whose err arm B is an inline scalar.  The
 * return bridge frees a caught result box after the carrier->concrete readback
 * copies its fields into the returned aggregate; a pointer/cstr/aggregate err
 * arm leaves the aggregate's err field aliasing the box's panic payload, so the
 * bridge must free only the box struct (shallow) and hand the payload to the
 * aggregate.  For a scalar err arm the aggregate copies err_val out as a plain
 * reinterpreted word that never points INTO the payload, so the FULL free
 * (`tur_result_box_free`, which also reclaims the 32 B payload on the err
 * branch) is safe -- and the ok branch has no payload, so it stays complete
 * either way.
 *
 * `t` is the return type the bridge already resolved via emit_resolve_type, so
 * this reads its app args purely and structurally (type_extract_adt_app -- no
 * emit_resolve_type / adt_field_type_for_app / type_adt_app_def call at the
 * return site).  That side-effect-free probe is deliberate: an earlier attempt
 * that recomputed the err-field type via those calls perturbed unrelated
 * rc-elision codegen (see the report). */
bool result_err_arm_is_freeable_scalar(const Type *t) {
    if (!t || t->kind != TY_APP) return false;
    AdtDef *def = NULL;
    Type args[16];
    uint8_t n_args = 0;
    if (!type_extract_adt_app(t, &def, args, &n_args) || !def) return false;
    if (!def->name || strcmp(def->name, "Result") != 0 || n_args != 2)
        return false;
    return err_val_result_is_freeable_scalar(args[1].kind);
}

/* Shared walker for the two scoped-free escape analyses.  `allow_box_accessors`
 * selects the catch-unwind result-box variant (catch-unwind-thunk-closure-leak):
 * a use of `b` as the sole argument of a read-only Result accessor -- `ok?`,
 * `err?`, or `ok-val` -- does NOT count as an escape, because none of those
 * retain the box or hand back box-owned-and-freed memory (ok?/err? return a
 * bool; ok-val copies out box->ok_val, which tur_result_box_free never frees).
 * `err-val` is whitelisted ONLY when its result type is a scalar
 * (err_val_result_is_freeable_scalar): it returns the box's panic-payload
 * pointer, which tur_result_box_free DOES free, so a pointer/cstr/aggregate err
 * arm could dangle an extracted payload and stays an escape; a scalar err arm
 * copies out a plain word that never aliases the payload, so freeing the box is
 * sound.  With the flag off this is exactly the fat-closure-env analysis. */
static bool binding_escapes_impl_x(const Expr *e, const Binding *b,
                                   bool allow_box_accessors,
                                   const Expr *ignore, bool allow_any_cast);

/* RM1 (reclamation-plan): widens the accessor whitelist for the sum-carrier
 * scope drop.  Every name here READS its Option/Result argument and returns a
 * copy of a payload word or a bool -- never a pointer INTO the box -- so the
 * box may be freed at scope exit after such uses.  The catch-box caller keeps
 * its narrower set: its free is DEEP (tur_result_box_free walks the payload),
 * which is why err-val is scalar-restricted there and unrestricted here. */
static bool g_esc_allow_sum_accessors = false;
/* closure-let-in-self-tail-loop-leaks: when set, the only use that is not an
 * escape is an invocation -- the `^borrow` / inferred non-retaining parameter
 * and borrowed-rest relaxations are off.  Those are sound for a free at scope
 * exit, but not for one at a self tail call's backedge: there the argument is
 * the next turn's parameter.  File-scope like the flag above. */
static bool g_esc_call_head_only = false;
/* dynamic-returned-closure-env-is-never-freed (self application): bit i set
 * admits `b` as argument i of a dynamic call whose CALLEE is `b` itself.  See
 * any_box_binding_escapes_self_apply.  File-scope for the same reason as the
 * flag above: one caller sets it, and the walk is not reentrant. */
static uint32_t g_esc_self_apply_mask = 0;
bool sum_box_reader_name(const char *nm) {
    /* Concrete stdlib defns only, each body audited for "reads, never
     * retains": the accessors copy a payload word or return a bool; the two
     * eq? comparators read both boxes through match / inline-C and return a
     * bool; option-map reads its input and mints a fresh result.  A CLASS
     * METHOD name (bind, fmap, eq?) must never appear here -- the callee is
     * dictionary-dispatched, and a user instance could retain its argument,
     * turning the drop into a use-after-free. */
    static const char *const names[] = {
        "some?", "none?", "ok?", "err?",
        "unwrap", "unwrap-or", "ok-val", "err-val",
        "result-eq?", "option-eq?", "option-map", NULL,
    };
    for (int i = 0; names[i]; i++)
        if (strcmp(nm, names[i]) == 0) return true;
    return false;
}

static bool binding_escapes_impl(const Expr *e, const Binding *b,
                                 bool allow_box_accessors,
                                 const Expr *ignore) {
    return binding_escapes_impl_x(e, b, allow_box_accessors, ignore, false);
}

/* any-struct-box-leak-per-widen: `allow_any_cast` additionally treats
 * `(cast b T)` on a bare `b` as a non-escape.  Sound for THIS decision and only
 * this one: the box is freed only when the tag says a widen heap-boxed the
 * payload, and for such a payload the cast emits `*(T *)TUR_UNTAG(v)` -- a copy,
 * which cannot alias the box.  For any other payload the drop is a no-op, so
 * whether the cast result aliases is irrelevant.  The flag is off for the
 * closure-env and catch-box callers, where a cast result can matter. */
static bool binding_escapes_impl_x(const Expr *e, const Binding *b,
                                   bool allow_box_accessors,
                                   const Expr *ignore, bool allow_any_cast) {
    if (!e || !b) return true;
    size_t cap = 256;
    const Expr **stack = (const Expr **)malloc(cap * sizeof(const Expr *));
    if (!stack) { fprintf(stderr, "tur: oom\n"); abort(); }
    int sp = 0;
    bool escapes = false;

#define ESC_PUSH(x) do {                                                       \
        const Expr *_x = (x);                                                  \
        if (_x) {                                                              \
            if ((size_t)sp >= cap) {                                           \
                cap *= 2;                                                       \
                stack = (const Expr **)realloc(stack, cap * sizeof(Expr *));   \
                if (!stack) { fprintf(stderr, "tur: oom\n"); abort(); }        \
            }                                                                  \
            stack[sp++] = _x;                                                  \
        }                                                                      \
    } while (0)

    ESC_PUSH(e);
    while (sp > 0) {
        const Expr *cur = stack[--sp];
        /* Part B: the caller's return-tail use of `b` -- already accounted for
         * (its value is copied out before the free) -- is not an escape. */
        if (cur == ignore) continue;
        switch (cur->kind) {
            case EX_VAR:
                if (cur->as.var.binding == b) { escapes = true; goto esc_done; }
                break;
            /* any-struct-box-leak-per-widen: `(type-of b)` and `(is? b T)` READ
             * the tag and yield something that cannot alias the payload -- a
             * bool, or a static name out of the runtime's table.  A bare `b`
             * directly underneath one is therefore not an escape, exactly as a
             * bare `b` under the ok?/err? accessors above is not.  A NESTED use
             * (`(is? (f b) T)`) is still walked and still escapes.
             *
             * EX_ANY_CAST is deliberately NOT here: it hands back the payload,
             * which for a pointer payload is the value itself, so a cast result
             * can alias `b` and the conservative default is the right answer. */
            case EX_ANY_CAST:
                if (allow_any_cast) {
                    const Expr *cop = cur->as.any_cast_.value;
                    while (cop && cop->kind == EX_ASCRIBE) cop = cop->as.ascribe_.inner;
                    if (cop && cop->kind == EX_VAR && cop->as.var.binding == b)
                        break;   /* a copy out of the box, not an escape of it */
                    ESC_PUSH(cur->as.any_cast_.value);
                    break;
                }
                escapes = true; goto esc_done;
            case EX_ANY_TYPE_OF:
            case EX_ANY_IS: {
                const Expr *op = (cur->kind == EX_ANY_TYPE_OF)
                                     ? cur->as.any_type_of_.value
                                     : cur->as.any_is_.value;
                while (op && op->kind == EX_ASCRIBE) op = op->as.ascribe_.inner;
                if (!(op && op->kind == EX_VAR && op->as.var.binding == b))
                    ESC_PUSH(op);
                break;
            }
            /* A direct call `(b ...)` is the one allowed, non-escaping use of
             * `b`: the callee is carried in fn_binding (not an EX_VAR child), so
             * it is simply not pushed here.  An indirect call whose callee slot
             * is the bare value `b` (fn_expr == EX_VAR(b)) is likewise an
             * invocation, not retention -- skip that child too.  Everything else
             * (args, dict arg, a non-`b` callee) is walked normally. */
            case EX_CALL: {
                const Expr *fe = cur->as.call_.fn_expr;
                if (fe && !(fe->kind == EX_VAR && fe->as.var.binding == b))
                    ESC_PUSH(fe);
                /* Box-accessor whitelist: a direct call to `ok?`/`err?`/`ok-val`
                 * reads its Result argument without retaining the box, so a `b`
                 * argument there is not an escape -- skip pushing it. */
                bool box_accessor = false;
                if (allow_box_accessors && !fe) {
                    const Binding *fb = cur->as.call_.fn_binding;
                    const char *nm = (fb && fb->name) ? fb->name->name : NULL;
                    box_accessor = nm && (strcmp(nm, "ok?") == 0 ||
                                          strcmp(nm, "err?") == 0 ||
                                          strcmp(nm, "ok-val") == 0);
                    if (!box_accessor && g_esc_allow_sum_accessors && nm)
                        box_accessor = sum_box_reader_name(nm);
                    /* Part A: err-val is a non-escape only when its scalar result
                     * cannot alias the payload tur_result_box_free reclaims. */
                    if (!box_accessor && nm && strcmp(nm, "err-val") == 0 &&
                        err_val_result_is_freeable_scalar(cur->type.kind))
                        box_accessor = true;
                }
                for (uint32_t i = 0; i < cur->as.call_.n_args; i++) {
                    const Expr *arg = cur->as.call_.args[i];
                    /* value-struct-payload-sum-monomorph-box-has-no-owner: a
                     * carrier argument to a polymorphic accessor arrives under
                     * an EX_ASCRIBE / non-retaining EX_REINTERPRET wrap (the
                     * int64 carrier re-typed for the tyvar slot); the wrap is a
                     * re-typing of the same word, not a use of it, so peel it
                     * before asking whether this argument is `b`.  An rc-RETAIN
                     * reinterpret is a real retention and is left in place. */
                    const Expr *barg = arg;
                    for (;;) {
                        if (!barg) break;
                        if (barg->kind == EX_ASCRIBE) barg = barg->as.ascribe_.inner;
                        else if (barg->kind == EX_REINTERPRET && !barg->as.reinterpret_.retain)
                            barg = barg->as.reinterpret_.expr;
                        /* RM1 (bind chains): the poly / fat packing around a
                         * continuation is a re-packing of the same env, not a
                         * use of it; a non-retaining slot does not retain it
                         * through the wrap either. */
                        else if (barg->kind == EX_POLY_WRAP) barg = barg->as.poly_wrap_.inner;
                        else if (barg->kind == EX_FN_TO_FAT) barg = barg->as.fn_to_fat_.inner;
                        else if (barg->kind == EX_POLY_TO_FAT) barg = barg->as.poly_to_fat_.inner;
                        else break;
                    }
                    bool arg_is_b = barg && barg->kind == EX_VAR &&
                                    barg->as.var.binding == b;
                    if (box_accessor && arg_is_b)
                        continue;
                    /* ... and a USER callee whose body was inferred not to
                     * retain this sum-typed parameter (nonretain_sum_param_mask)
                     * confines it exactly as the audited readers do.  Only under
                     * the sum walk: the mask is a statement about the arm box,
                     * which is what that walk's client frees. */
                    if (g_esc_allow_sum_accessors && arg_is_b && !fe &&
                        call_dispatch_is_static(cur)) {
                        const Binding *sfb = cur->as.call_.fn_binding;
                        if (sfb && i < 32 &&
                            (sfb->nonretain_sum_param_mask & (1u << i)))
                            continue;
                    }
                    /* closure-drop-glue S1: a `^borrow` fn-param (FA_BORROW) is
                     * borrowed, not retained -- the callee invokes but does not
                     * store/return it.  So `b` passed to a borrowed param does NOT
                     * escape and its env may be freed at scope exit.  Same soundness
                     * posture as the box-accessor whitelist (only greenlights a
                     * free); relies on the callee honouring its ^borrow contract.
                     *
                     * closure-drop-glue S1c: the same relaxation applies to an
                     * INFERRED non-retaining fn-param (nonretain_param_mask bit i)
                     * -- a fn-typed / ^fat param the callee body only CALLS.  This
                     * covers the common `^fat h` consuming-callee shape that carries
                     * no `^borrow` annotation.  Soundness rides the same escape
                     * analysis that set the bit: if the callee let the closure
                     * escape, the bit is clear and the arg is walked as an escape. */
                    if (arg_is_b && call_dispatch_is_static(cur) && !g_esc_call_head_only) {
                        const Binding *fb = cur->as.call_.fn_binding;
                        if (fb && fb->type.kind == TY_FN
                            && i < fb->type.as.fn.arity
                            && fb->type.as.fn.arg_flags
                            && FN_ARG_FLAG(fb->type.as.fn, i, FA_BORROW))
                            continue;
                        if (fb && i < 32 &&
                            (fb->nonretain_param_mask & (1u << i)))
                            continue;
                    }
                    /* closure-drop-glue (mw-compose-of): a `^borrow & rest` callee
                     * reads/invokes each rest element but retains none.  The rest
                     * param is the last declared param (index arity-1); any argument
                     * at or past it is a rest element, so `b` passed there does NOT
                     * escape -- the caller frees its env at scope exit (once per
                     * binding, so passing the same value twice cannot double-free,
                     * unlike a callee-side per-apply free).  The rest arguments are
                     * collected into a single EX_CONS_LIST node whose items reach the
                     * cons builder wrapped in carrier casts (EX_CAST / EX_ASCRIBE /
                     * fat/poly coercions); walk the items, skip an item that peels to
                     * `b` (borrowed, non-escaping), and push the rest. */
                    if (arg && arg->kind == EX_CONS_LIST && !g_esc_call_head_only) {
                        const Binding *fb = cur->as.call_.fn_binding;
                        if (fb && fb->type.kind == TY_FN
                            && fb->type.as.fn.is_variadic
                            && fb->type.as.fn.rest_borrow
                            && fb->type.as.fn.arity >= 1
                            && i >= fb->type.as.fn.arity - 1) {
                            for (uint32_t ci = 0; ci < arg->as.cons_list_.n; ci++) {
                                const Expr *pa = arg->as.cons_list_.items[ci];
                                while (pa) {
                                    if (pa->kind == EX_ASCRIBE) pa = pa->as.ascribe_.inner;
                                    else if (pa->kind == EX_CAST) pa = pa->as.cast_.expr;
                                    else if (pa->kind == EX_FN_TO_FAT) pa = pa->as.fn_to_fat_.inner;
                                    else if (pa->kind == EX_POLY_TO_FAT) pa = pa->as.poly_to_fat_.inner;
                                    else if (pa->kind == EX_POLY_WRAP) pa = pa->as.poly_wrap_.inner;
                                    else break;
                                }
                                if (pa && pa->kind == EX_VAR && pa->as.var.binding == b)
                                    continue; /* borrowed rest element -- no escape */
                                ESC_PUSH(arg->as.cons_list_.items[ci]);
                            }
                            continue; /* handled the cons-list arg element-wise */
                        }
                    }
                    ESC_PUSH(arg);
                }
                ESC_PUSH(cur->as.call_.dict_arg);
                break;
            }
            /* A nested closure that captures `b` makes it escape: do not descend
             * (the body accesses captures through its own env), just consult the
             * precomputed capture set. */
            case EX_CLOSURE:
                if (cur->as.closure_.closure) {
                    struct Closure *c = cur->as.closure_.closure;
                    for (uint32_t i = 0; i < c->n_captures; i++)
                        if (c->captures[i] == b) { escapes = true; goto esc_done; }
                }
                break;
            case EX_FN_DEF:
                if (cur->as.fn_def_.fn && cur->as.fn_def_.fn->closure) {
                    struct Closure *c = cur->as.fn_def_.fn->closure;
                    for (uint32_t i = 0; i < c->n_captures; i++)
                        if (c->captures[i] == b) { escapes = true; goto esc_done; }
                }
                break;
            /* A defer body runs at scope exit -- the same point as the env free --
             * but it can only reference `b` through its precomputed capture set
             * (the body is lifted into a thunk that reaches enclosing locals via
             * `captures`, exactly like EX_CLOSURE/EX_FN_DEF).  So consult that set:
             * `b` captured -> the defer uses the closure at scope exit -> escape;
             * `b` NOT captured -> the defer cannot touch the closure, and freeing
             * its env is safe regardless of the shared scope-exit ordering.
             *
             * fat-closure-env-leak-with-owning-sibling: an owning let-binding
             * (rc/ref) injects its auto-drop as a `(defer (drop r))` into the let
             * body.  The prior blanket `default: escape` for EX_DEFER therefore
             * flagged EVERY sibling closure as escaping whenever an owning binding
             * was present, so the closure env leaked (16 B/construction) in any let
             * that also bound an rc/ref -- even when the closure captured only
             * scalars.  Consulting the capture set fixes that without ever
             * greenlighting a free of an env the defer actually uses. */
            case EX_DEFER:
                for (uint8_t i = 0; i < cur->as.defer_.n_captures; i++)
                    if (cur->as.defer_.captures[i] == b) { escapes = true; goto esc_done; }
                break;
            /* Inline-C may name `b` through its capture array (__TUR_CAP_N__) in
             * addition to its evaluated sub-expressions. */
            case EX_INLINE_C: {
                InlineC *ic = cur->as.inline_c_.inline_c;
                if (ic) {
                    for (uint8_t i = 0; i < ic->n_captures; i++)
                        if (ic->captures[i] == b) { escapes = true; goto esc_done; }
                    for (uint8_t i = 0; i < ic->n_val_exprs; i++)
                        ESC_PUSH(ic->val_exprs[i]);
                }
                break;
            }

            /* ---- container kinds: walk every sub-expression ---- */
            case EX_LET:
            case EX_LETREC:
                for (uint32_t i = 0; i < cur->as.let_.n; i++)
                    ESC_PUSH(cur->as.let_.bindings[i].init);
                ESC_PUSH(cur->as.let_.body);
                break;
            case EX_IF:
                ESC_PUSH(cur->as.if_.cond);
                ESC_PUSH(cur->as.if_.then_);
                ESC_PUSH(cur->as.if_.else_or_null);
                break;
            case EX_DO:
                for (uint32_t i = 0; i < cur->as.do_.n; i++)
                    ESC_PUSH(cur->as.do_.items[i]);
                break;
            case EX_WHILE:
                ESC_PUSH(cur->as.while_.cond);
                ESC_PUSH(cur->as.while_.body);
                break;
            case EX_SET:
                /* Reassigning `b` itself: the scope-exit drop would then release
                 * whatever `b` holds LAST -- possibly a value some other holder
                 * owns -- and never the one this scope minted.  Decline. */
                if (cur->as.set_.target == b) { escapes = true; goto esc_done; }
                ESC_PUSH(cur->as.set_.value);
                break;
            /* dynamic-returned-closure-env-is-never-freed: a dynamic call
             * `(b x)` whose CALLEE is the bare value `b` invokes it, exactly as
             * an EX_CALL whose fn_expr is `b` does -- invocation is not
             * retention.  The arguments are walked as they are for EX_CALL,
             * and with less to go on: the callee is unknown, so `b` passed as
             * an argument is an escape.  Before this, any dynamic call anywhere
             * in a scope made every binding in it escape, which is why no scope
             * drop ever fired in a dynamic file whose body called a value. */
            case EX_DYN_CALL: {
                const Expr *fe = cur->as.dyn_call_.fn;
                const Expr *pfe = fe;
                while (pfe && pfe->kind == EX_ASCRIBE) pfe = pfe->as.ascribe_.inner;
                bool callee_is_b = pfe && pfe->kind == EX_VAR && pfe->as.var.binding == b;
                if (!callee_is_b)
                    ESC_PUSH(fe);
                for (uint32_t i = 0; i < cur->as.dyn_call_.n_args; i++) {
                    if (callee_is_b && i < 32 && (g_esc_self_apply_mask & (1u << i))) {
                        const Expr *pa = cur->as.dyn_call_.args[i];
                        while (pa && pa->kind == EX_ASCRIBE) pa = pa->as.ascribe_.inner;
                        if (pa && pa->kind == EX_VAR && pa->as.var.binding == b)
                            continue;   /* handed back to its own code: see the mask */
                    }
                    ESC_PUSH(cur->as.dyn_call_.args[i]);
                }
                break;
            }
            /* A widen re-tags its operand's value, so `b` escapes through it
             * exactly when it escapes through the operand: `(:: b any)` reaches
             * EX_VAR and is an escape; `(:: (b 4) any)` is an invocation.  It
             * used to fall to `default`, which is how a dynamic file's
             * `(println (add3 4))` -- the call result widened for println --
             * read as an escape of add3. */
            case EX_UNION_INJECT:
                ESC_PUSH(cur->as.union_inject_.value);
                break;
            /* A runtime-dispatched operator: its operands are walked, so a bare
             * `b` operand is an escape (an operator such as `cons` stores it). */
            case EX_DYN_OP:
                for (uint32_t i = 0; i < cur->as.dyn_op_.n_args; i++)
                    ESC_PUSH(cur->as.dyn_op_.args[i]);
                break;
            case EX_BUILTIN:
                for (uint32_t i = 0; i < cur->as.builtin.n; i++)
                    ESC_PUSH(cur->as.builtin.args[i]);
                break;
            case EX_MAKE_STRUCT:
                for (uint32_t i = 0; i < cur->as.make_struct_.n_fields; i++)
                    ESC_PUSH(cur->as.make_struct_.field_values[i]);
                break;
            case EX_SET_LIT:
                for (uint32_t i = 0; i < cur->as.set_lit_.n; i++)
                    ESC_PUSH(cur->as.set_lit_.items[i]);
                break;
            case EX_CONS_LIST:
                for (uint32_t i = 0; i < cur->as.cons_list_.n; i++)
                    ESC_PUSH(cur->as.cons_list_.items[i]);
                break;
            case EX_GET_FIELD:
                ESC_PUSH(cur->as.get_field_.struct_expr);
                break;
            case EX_SET_FIELD:
                ESC_PUSH(cur->as.set_field_.receiver);
                ESC_PUSH(cur->as.set_field_.value);
                break;
            case EX_REF:        ESC_PUSH(cur->as.ref_.expr);        break;
            case EX_DEREF:      ESC_PUSH(cur->as.deref_.expr);      break;
            /* fat-closure-env-leak-with-owning-sibling: the rc/weak/ref-family
             * operations are single-operand nodes; walk the operand so `b` is
             * detected iff it actually flows into one (e.g. `(rc/of b)` stores the
             * closure into an rc -> escape).  Previously these fell to the
             * conservative `default: escape`, so a sibling OWNING binding whose
             * init is `(rc/of ...)` / `(weak ...)` / etc. was read as an escape of
             * EVERY sibling closure -- the exact reason a let that bound both an rc
             * and a capturing closure leaked the closure env. */
            case EX_RC_OF:       ESC_PUSH(cur->as.rc_of_.expr);       break;
            case EX_RC_CLONE:    ESC_PUSH(cur->as.rc_clone_.expr);    break;
            case EX_RC_DROP:     ESC_PUSH(cur->as.rc_drop_.expr);     break;
            case EX_RC_PTR:      ESC_PUSH(cur->as.rc_ptr_.expr);      break;
            case EX_RC_COUNT:    ESC_PUSH(cur->as.rc_count_.expr);    break;
            case EX_RC_FROM_REF: ESC_PUSH(cur->as.rc_from_ref_.expr); break;
            case EX_REF_FROM_RC: ESC_PUSH(cur->as.ref_from_rc_.expr); break;
            case EX_WEAK:         ESC_PUSH(cur->as.weak_.expr);         break;
            case EX_WEAK_UPGRADE: ESC_PUSH(cur->as.weak_upgrade_.expr); break;
            case EX_WEAK_PRED:    ESC_PUSH(cur->as.weak_pred_.expr);    break;
            case EX_REF_PRED:     ESC_PUSH(cur->as.ref_pred_.expr);     break;
            case EX_BORROW_IMMUT: ESC_PUSH(cur->as.borrow_immut_.expr); break;
            case EX_BORROW_MUT:   ESC_PUSH(cur->as.borrow_mut_.expr);   break;
            case EX_SET_DEREF:
                ESC_PUSH(cur->as.set_deref_.ref);
                ESC_PUSH(cur->as.set_deref_.value);
                break;
            /* EX_PERFORM is a continuation-capture point: a multi-shot handler
             * could resume the captured continuation -- which spans this scope's
             * trailing free -- more than once, double-freeing the shared env.
             * Conservatively report an escape so the env is left to leak rather
             * than risk a double free.  (shift/reset/await/call-cc and the other
             * capture forms are not modeled below and reach `default` -> escape.) */
            case EX_PERFORM:
                escapes = true;
                goto esc_done;
            case EX_HANDLE:
                ESC_PUSH(cur->as.handle_.handle->body);
                for (uint8_t i = 0; i < cur->as.handle_.handle->n_cases; i++)
                    ESC_PUSH(cur->as.handle_.handle->cases[i].body);
                break;
            case EX_RESUME:
                ESC_PUSH(cur->as.resume_.resume->k);
                ESC_PUSH(cur->as.resume_.resume->value);
                break;
            case EX_DISCONTINUE:
                ESC_PUSH(cur->as.discontinue_.discontinue->k);
                ESC_PUSH(cur->as.discontinue_.discontinue->exception);
                break;
            case EX_RETURN:
                ESC_PUSH(cur->as.return_.value);
                break;
            case EX_PANIC:
                ESC_PUSH(cur->as.panic_.payload);
                break;
            /* A catch boundary's only sub-expression is its thunk closure; a
             * reference to `b` there is caught via that closure's capture set
             * (EX_CLOSURE/EX_FN_DEF).  Walking it -- rather than hitting the
             * conservative default-escape below -- lets the catch-box analysis
             * see through `b`'s own `(catch-unwind ...)` initializer (Part B
             * sole-ownership), which the thunk never closes over.  Scoped to the
             * catch-box variant so the fat-closure-env analysis (which does not
             * reason about caught boxes) keeps its exact prior behavior. */
            case EX_CATCH_UNWIND:
                if (!allow_box_accessors) { escapes = true; goto esc_done; }
                ESC_PUSH(cur->as.catch_unwind_.thunk);
                break;
            case EX_CATCH_PANIC_OF:
                if (!allow_box_accessors) { escapes = true; goto esc_done; }
                ESC_PUSH(cur->as.catch_panic_of_.thunk);
                break;
            /* Fn->fat / poly coercion wrappers erase to their inner fn/closure at
             * codegen; a reference to `b` lives (if anywhere) in that inner
             * closure's capture set.  A catch-unwind thunk reaches the analysis
             * wrapped in EX_FN_TO_FAT, so the catch-box variant descends; the
             * fat-closure-env variant keeps defaulting to escape (unchanged). */
            /* RM1 (bind chains): the env variant now descends too.  A packing
             * re-packs its operand, so `b` escapes through it exactly when it
             * escapes through the operand -- a bare `b` inside reaches EX_VAR
             * and is still an escape, and `b` in a non-retaining slot was
             * already admitted by the call arm's peel.  Defaulting to escape
             * here made any lambda argument ANYWHERE in the body an escape of
             * an unrelated let-bound closure (`scale4` beside a literal). */
            case EX_FN_TO_FAT:
                ESC_PUSH(cur->as.fn_to_fat_.inner);
                break;
            case EX_POLY_TO_FAT:
                ESC_PUSH(cur->as.poly_to_fat_.inner);
                break;
            case EX_POLY_WRAP:
                ESC_PUSH(cur->as.poly_wrap_.inner);
                break;
            case EX_MATCH:
                ESC_PUSH(cur->as.match_.scrutinee);
                for (uint32_t i = 0; i < cur->as.match_.n_arms; i++) {
                    ESC_PUSH(cur->as.match_.arms[i].guard);
                    ESC_PUSH(cur->as.match_.arms[i].body);
                }
                break;
            case EX_ASCRIBE:
                ESC_PUSH(cur->as.ascribe_.inner);
                break;
            case EX_CAST:
                ESC_PUSH(cur->as.cast_.expr);
                break;
            /* A bit reinterpret re-types its operand's word; whether `b`
             * escapes through it is whether `b` escapes through the operand
             * (a bare reinterpret of `b` reaches EX_VAR and is an escape, as a
             * bare `b` is).  It used to fall to `default`, which reported every
             * carrier-typed accessor argument as an escape. */
            case EX_REINTERPRET:
                ESC_PUSH(cur->as.reinterpret_.expr);
                break;

            /* ---- leaves: cannot reference `b` ---- */
            case EX_NIL_LIT:
            case EX_BOOL_LIT:
            case EX_INT_LIT:
            case EX_FLOAT_LIT:
            case EX_CSTR_LIT:
            case EX_SYM_LIT:
            case EX_DICT:
                break;

            default:
                /* Unknown / unmodeled kind: conservatively treat as an escape so
                 * we never free an env that is still reachable. */
                escapes = true;
                goto esc_done;
        }
    }
esc_done:
#undef ESC_PUSH
    free(stack);
    return escapes;
}

bool closure_binding_escapes(const Expr *e, const Binding *b) {
    return binding_escapes_impl(e, b, /*allow_box_accessors=*/false, NULL);
}

/* closure-let-in-self-tail-loop-leaks: is every use of `b` in `e` a call of
 * it?  closure_binding_escapes with the argument relaxations off. */
bool closure_binding_only_invoked(const Expr *e, const Binding *b) {
    g_esc_call_head_only = true;
    bool esc = binding_escapes_impl(e, b, /*allow_box_accessors=*/false, NULL);
    g_esc_call_head_only = false;
    return !esc;
}

/* catch-unwind-thunk-closure-leak: true if the caught-Result binding `b` is used
 * anywhere in `e` other than as a read-only `ok?`/`err?`/`ok-val` argument -- so
 * a false return means the box provably does not escape and may be freed at
 * scope exit.  Same soundness posture as closure_binding_escapes (only ever
 * greenlights a free). */
bool catch_box_binding_escapes(const Expr *e, const Binding *b) {
    return binding_escapes_impl(e, b, /*allow_box_accessors=*/true, NULL);
}

/* RM1: the catch-box walk with the sum-accessor whitelist widened.  The flag
 * is file-scope rather than a sixth parameter because exactly one caller sets
 * it and the walk is not reentrant (iterative, no callbacks). */
bool sum_box_binding_escapes(const Expr *e, const Binding *b) {
    g_esc_allow_sum_accessors = true;
    bool r = binding_escapes_impl(e, b, /*allow_box_accessors=*/true, NULL);
    g_esc_allow_sum_accessors = false;
    return r;
}

bool catch_box_binding_escapes_except(const Expr *e, const Binding *b,
                                      const Expr *ignore) {
    return binding_escapes_impl(e, b, /*allow_box_accessors=*/true, ignore);
}

/* any-struct-box-leak-per-widen: the two walks above, with `(cast b T)` on a
 * bare `b` admitted as a read.  Used only by the `any` drop rules. */
bool any_box_binding_escapes(const Expr *e, const Binding *b) {
    return binding_escapes_impl_x(e, b, /*allow_box_accessors=*/true, NULL,
                                  /*allow_any_cast=*/true);
}

bool any_box_binding_escapes_except(const Expr *e, const Binding *b,
                                    const Expr *ignore) {
    return binding_escapes_impl_x(e, b, /*allow_box_accessors=*/true, ignore,
                                  /*allow_any_cast=*/true);
}

/* dynamic-returned-closure-env-is-never-freed (self application): the `any`
 * walk, but a dynamic call `(b ... b ...)` whose callee is `b` may also take
 * `b` as argument i when bit i of `self_mask` is set.
 *
 * That is only sound when the code `b` runs does not keep what arrives in
 * slot i -- which is what the mask says.  It has two users that together make
 * the argument:
 *
 *   - elab_fns asks it of a fresh closure's OWN body, one parameter at a time
 *     with just that parameter's bit, and sets the bit in the producer's
 *     fresh_closure_self_apply_mask when the parameter does not escape.  The
 *     parameter may then be invoked, and handed back to itself in the same
 *     slot, and nothing else: no store, no return, no capture, no other slot.
 *   - the scope drop asks it of the let body with that mask.
 *
 * `(counter counter 1000 0)` then puts the closure C in slot 0 of C.  Inside,
 * the only handle C has on itself is that parameter, and it can do nothing with
 * it but call C again with C in slot 0 -- the same situation, one level down.
 * So no activation reached from the call can store or return C, and the scope
 * that minted C is its only owner when the call returns.  An activation that
 * calls its parameter with something ELSE in slot 0 runs C with no handle on C
 * at all, which is safer still.  Everything else about the walk is unchanged. */
bool any_box_binding_escapes_self_apply(const Expr *e, const Binding *b,
                                        uint32_t self_mask) {
    g_esc_self_apply_mask = self_mask;
    bool r = binding_escapes_impl_x(e, b, /*allow_box_accessors=*/true, NULL,
                                    /*allow_any_cast=*/true);
    g_esc_self_apply_mask = 0;
    return r;
}

/* dynamic-returned-closure-env-is-never-freed: is releasing this closure's env
 * through its drop-glue header a free of the env block and nothing else?
 * drop_glue_<env> also releases an rc capture, an owned `^fat` capture and a
 * capture with a Drop instance; each of those could be a value a call of the
 * closure handed back, so a closure that has one is not admitted.  An `any`
 * capture is copied into the env word for word and its glue leaves it alone;
 * a scalar has nothing to release.  A body with inline C is refused too: it is
 * the only way a closure's code can reach its own env and hand it out. */
static bool fresh_closure_capture_kind_ok(TypeKind k) {
    switch (k) {
        case TY_ANY:
        case TY_INT: case TY_BOOL: case TY_FLOAT: case TY_CSTR: case TY_NIL:
        case TY_PTR_VOID:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return true;
        default:
            return false;
    }
}

bool closure_env_drop_is_shallow(const struct Closure *c) {
    if (!c || !c->fn || c->n_captures == 0) return false;
    if (c->is_shift_receiver || c->is_effect_payload) return false;
    if (expr_subtree_has_inline_c(c->fn->body)) return false;
    uint32_t heap_caps = 0;
    for (uint32_t i = 0; i < c->n_captures; i++) {
        const Binding *cap = c->captures[i];
        if (!cap) return false;
        if (cap->is_global) continue;          /* no env field */
        heap_caps++;
        if (cap->is_fat) return false;
        if (c->capture_drop_insts && c->capture_drop_insts[i]) return false;
        if (!fresh_closure_capture_kind_ok(cap->type.kind)) return false;
    }
    return heap_caps > 0;                      /* an env block exists */
}

/* Does an inline-C block in `e` name one of closure `c`'s captures?  Inside a
 * lifted lambda a captured binding is read from the env box, so a C block
 * that names it (`__TUR_CAP_N__`) holds an lvalue INTO the box and may take
 * its address; one that names none -- a stdlib `tur_session_recv(__TUR_VAL_0__)`,
 * whose sub-expressions are evaluated to values -- cannot reach the box.
 * Walked exactly, through the shared operand enumeration (which does not
 * enter a nested fn: its body is its own), where expr_subtree_has_inline_c
 * reads every node it does not model -- a `perform`, an `await` -- as
 * possible inline C. */
typedef struct { const struct Closure *c; bool found; } IcEnvUd;
static bool inline_c_env_visit(const Expr *x, void *ud);
static bool inline_c_touches_env(const Expr *e, const struct Closure *c) {
    if (!e) return false;
    if (e->kind == EX_INLINE_C) {
        const InlineC *ic = e->as.inline_c_.inline_c;
        if (!ic) return true;
        for (uint8_t i = 0; i < ic->n_captures; i++)
            for (uint32_t k = 0; k < c->n_captures; k++)
                if (ic->captures[i] && ic->captures[i] == c->captures[k]) return true;
        return false;
    }
    IcEnvUd u = { c, false };
    cps_visit_children(e, inline_c_env_visit, &u);
    return u.found;
}
static bool inline_c_env_visit(const Expr *x, void *ud) {
    IcEnvUd *u = (IcEnvUd *)ud;
    if (inline_c_touches_env(x, u->c)) { u->found = true; return true; }
    return false;
}
bool closure_body_inline_c_touches_env(const struct Closure *c) {
    return c && c->fn && inline_c_touches_env(c->fn->body, c);
}

/* async-capturing-body-env-never-freed: may whoever alone owns this fresh
 * closure release its env once the body is done, though the body's result is
 * still live?  The env's drop glue (emit_expr.c, emit_fns.c) releases only an
 * rc capture, an owned `^fat` closure capture and a Drop-instance capture,
 * then frees the box.  With none of those it frees the box alone, which no
 * value the body computes points into -- unless inline C took an address in
 * it.  A struct capture is a copy in the box, so it qualifies; the shallow
 * test above does not take it. */
bool closure_env_drop_frees_box_only(const struct Closure *c) {
    if (!c || !c->fn || c->n_captures == 0) return false;
    if (c->is_shift_receiver || c->is_effect_payload) return false;
    uint32_t heap_caps = 0;
    for (uint32_t i = 0; i < c->n_captures; i++) {
        const Binding *cap = c->captures[i];
        if (!cap) return false;
        if (cap->is_global) continue;
        heap_caps++;
        if (cap->type.kind == TY_RC || cap->is_fat) return false;
        if (c->capture_drop_insts && c->capture_drop_insts[i]) return false;
    }
    return heap_caps > 0 && !closure_body_inline_c_touches_env(c);
}

/* dynamic-returned-closure-env-is-never-freed: does `x` evaluate to an `any`
 * holding a capturing closure's env that nothing but this value owns?  Two
 * shapes: a widen of a capturing lambda written right here, and a call to a
 * function whose tail is one (returns_fresh_any_closure).  Either way the env
 * was malloc'd behind a drop-glue header for this value alone, so whoever
 * owns the value may release it with TUR_CLOSURE_DROP. */
bool expr_is_fresh_any_closure(const Expr *x) {
    while (x && x->kind == EX_ASCRIBE) x = x->as.ascribe_.inner;
    if (!x) return false;
    if (x->kind == EX_CALL)
        return x->as.call_.fn_binding &&
               x->as.call_.fn_binding->returns_fresh_any_closure;
    if (x->kind != EX_UNION_INJECT || x->type.kind != TY_ANY ||
        x->as.union_inject_.frame_box)
        return false;
    const Expr *v = x->as.union_inject_.value;
    while (v && (v->kind == EX_ASCRIBE || v->kind == EX_FN_TO_FAT ||
                 v->kind == EX_POLY_TO_FAT || v->kind == EX_POLY_WRAP))
        v = v->kind == EX_ASCRIBE     ? v->as.ascribe_.inner
          : v->kind == EX_FN_TO_FAT   ? v->as.fn_to_fat_.inner
          : v->kind == EX_POLY_TO_FAT ? v->as.poly_to_fat_.inner
                                      : v->as.poly_wrap_.inner;
    return v && v->kind == EX_CLOSURE &&
           closure_env_drop_is_shallow(v->as.closure_.closure);
}

/* catch-unwind-panic-payload-leaks (Leak 2): runtime sinks that CONSUME their
 * argument -- print it -- and never retain a pointer into it beyond the call.
 * A box-owned pointer (the caught message an err-val / inline-C accessor hands
 * back) passed to one of these is dead once the call returns, so the box may be
 * deep-freed at scope exit.  A general call is NOT on this list: it could stash
 * the pointer invisibly (e.g. through an inline-C body), so it never confines. */
static bool box_reader_result_void_sink(const char *name) {
    if (!name) return false;
    static const char *const sinks[] = {
        "println", "print", "eprintln", "eprint",
        "println-float", "printf-float", "print-show", "print-char",
        "print-char-list", NULL,
    };
    for (int i = 0; sinks[i]; i++)
        if (strcmp(name, sinks[i]) == 0) return true;
    return false;
}

/* catch-unwind-panic-payload-leaks (Leak 2): is every use of caught-box binding
 * `b` in `e` safe for a DEEP box free (box struct + owned payload/message) at
 * b's scope exit?  Beyond the scalar-accessor whitelist that
 * catch_box_binding_escapes admits, this also admits handing `b` to a reader
 * (e.g. the fixture's inline-C `panic-msg`, which returns a pointer INTO the
 * box-owned message) PROVIDED the reader's result is `confined` -- consumed
 * within the scope so no box-owned pointer is live at the free point.
 *
 * `confined` is true when the value of `e` is discarded or flows only into a
 * known non-retaining runtime sink (the print family above).  It must start
 * true only for a box scope whose own result cannot carry a box-owned pointer
 * out (a non-pointer scalar / nil scope value); the caller enforces that.
 *
 * Soundness (only ever greenlights a free):
 *   - a reader result reaching a store / return / capture is caught because that
 *     context recurses with confined=false (or, for a `b` appearing directly,
 *     falls to the strict escape walk), and an unconfined reader use returns
 *     false;
 *   - a reader result BOUND to a local is caught: a let-binding init is walked
 *     with confined=false, so the alias cannot later escape unnoticed;
 *   - an unmodeled form defers to the strict escape walk, whose own default is
 *     to treat the unknown as an escape. */
/* byvalue-recursive-adt-boxes-are-never-freed (Residue 1): the walk's
 * alias-aware STRICT mode, used for a by-value recursive ADT parameter whose
 * spine the CALLER keeps and frees at scope exit when the callee is proven
 * non-retaining.  Two things the pointer/any/sum modes do not model matter
 * here, because the free is a deep one:
 *
 *   - a `match` on the parameter binds ALIASES into its spine -- `t` in
 *     `(match xs (Cons h t) ...)` carries the next box's pointer -- and so
 *     does a field read (`(.tl xs)`).  A use of such an alias is a use of the
 *     parameter: stored, returned, let-bound or handed to an opaque callee it
 *     is retention.  Binders of non-pointer scalar type (`h : int`) cannot
 *     point into the spine and are not tracked.
 *   - a hand-off to a callee is an escape unless that callee's own mask says
 *     it does not retain the argument (the sum walk's posture, not the
 *     pointer walk's "reader model", since the callee could store it).
 *
 * The alias set is a file-scope stack, like the sum flag: the walk is not
 * reentrant.  Bounded; overflow refuses (conservative). */
static bool g_bc_strict_handoff = false;
/* byvalue-recursive-shared-copies-leak: the walk's root is a ^borrow
 * parameter.  Nothing derived from a borrow is ever an OWNER (ownership
 * provenance refuses it), so a local bound to any value that may point into
 * the root's spine is only an alias to track -- where for an owned root a
 * bare rebinding is a move, and its local becomes the spine's owner. */
static bool g_bc_root_unowned = false;
#define BC_MAX_ALIAS 64
static const Binding *g_bc_alias[BC_MAX_ALIAS];
static uint32_t g_bc_n_alias = 0;
static bool g_bc_alias_overflow = false;

static bool bc_is_b(const Binding *x, const Binding *b) {
    if (x == b) return true;
    for (uint32_t i = 0; i < g_bc_n_alias; i++)
        if (g_bc_alias[i] == x) return true;
    return false;
}

/* A value of this kind may carry a pointer into the spine (or into an owned
 * payload box): anything but a non-pointer scalar. */
static bool bc_kind_can_alias(TypeKind k) {
    switch (k) {
        case TY_NIL: case TY_BOOL: case TY_INT: case TY_FLOAT: case TY_SYM:
        case TY_INT8: case TY_INT16: case TY_INT32: case TY_INT64:
        case TY_UINT8: case TY_UINT16: case TY_UINT32: case TY_UINT64:
        case TY_FLOAT32: case TY_FLOAT64:
            return false;
        default:
            return true;
    }
}

static void bc_push_alias(const Binding *x) {
    if (!x) return;
    if (g_bc_n_alias >= BC_MAX_ALIAS) { g_bc_alias_overflow = true; return; }
    g_bc_alias[g_bc_n_alias++] = x;
}

/* Peel ascriptions, casts and field reads down to the root variable: true
 * when that root is `b` or a tracked alias of it. */
static bool bc_expr_roots_at_b(const Expr *x, const Binding *b) {
    while (x) {
        switch (x->kind) {
            case EX_ASCRIBE:   x = x->as.ascribe_.inner; continue;
            case EX_CAST:      x = x->as.cast_.expr; continue;
            case EX_GET_FIELD: x = x->as.get_field_.struct_expr; continue;
            case EX_VAR:       return bc_is_b(x->as.var.binding, b);
            default:           return false;
        }
    }
    return false;
}

/* byvalue-recursive-shared-copies-leak: is `x` a static call handing `b` (or
 * an alias of it) to a parameter the callee keeps only in its result? */
static bool bc_call_result_aliases_b(const Expr *x, const Binding *b) {
    while (x && (x->kind == EX_ASCRIBE || x->kind == EX_CAST))
        x = x->kind == EX_ASCRIBE ? x->as.ascribe_.inner : x->as.cast_.expr;
    if (!x || x->kind != EX_CALL || x->as.call_.fn_expr) return false;
    const Binding *fb = x->as.call_.fn_binding;
    if (!fb || !fb->resalias_param_mask || !call_dispatch_is_static(x)) return false;
    for (uint32_t i = 0; i < x->as.call_.n_args && i < 32; i++) {
        const Expr *a = x->as.call_.args[i];
        if (a && a->kind == EX_VAR && bc_is_b(a->as.var.binding, b) &&
            (fb->resalias_param_mask & (1u << i)))
            return true;
    }
    return false;
}

/* The unmodelled-form answer: defer to the strict escape walk for `b`, and in
 * strict mode for every tracked alias too. */
static bool bc_unmodelled(const Expr *e, const Binding *b) {
    if (catch_box_binding_escapes(e, b)) return false;
    if (g_bc_strict_handoff) {
        if (g_bc_alias_overflow) return false;
        for (uint32_t i = 0; i < g_bc_n_alias; i++)
            if (catch_box_binding_escapes(e, g_bc_alias[i])) return false;
    }
    return true;
}

static bool box_uses_confined(const Expr *e, const Binding *b, bool confined) {
    if (!e) return true;
    switch (e->kind) {
        case EX_INT_LIT: case EX_BOOL_LIT: case EX_FLOAT_LIT:
        case EX_CSTR_LIT: case EX_NIL_LIT: case EX_SYM_LIT:
            return true;
        case EX_VAR:
            /* A bare `b` as this expression's result is safe only if confined --
             * its value (the box pointer) is then discarded / printed, not kept. */
            return !bc_is_b(e->as.var.binding, b) || confined;
        case EX_GET_FIELD:
            if (!g_bc_strict_handoff) return bc_unmodelled(e, b);
            /* Strict mode: a field read rooted at the parameter yields an
             * alias unless the field is a non-pointer scalar, so it stands
             * where a bare `b` would. */
            if (bc_expr_roots_at_b(e->as.get_field_.struct_expr, b))
                return !bc_kind_can_alias(e->type.kind) || confined;
            return box_uses_confined(e->as.get_field_.struct_expr, b, true);
        case EX_REINTERPRET: return box_uses_confined(e->as.reinterpret_.expr, b, confined);
        case EX_ASCRIBE: return box_uses_confined(e->as.ascribe_.inner, b, confined);
        case EX_CAST:    return box_uses_confined(e->as.cast_.expr, b, confined);
        /* byvalue-recursive-shared-copies-leak: an rc clone takes a count on
         * its operand and hands the same control block back -- transparent,
         * like a widen, in the position it occupies. */
        case EX_RC_CLONE: return box_uses_confined(e->as.rc_clone_.expr, b, confined);
        /* ... and a count DROP, or the scope-exit defer the rc field auto-drop
         * wraps it in, keeps nothing: its operand is read and released, its
         * value discarded. */
        case EX_RC_DROP:  return box_uses_confined(e->as.rc_drop_.expr, b, /*confined=*/true);
        case EX_DEFER:    return box_uses_confined(e->as.defer_.body, b, /*confined=*/true);
        /* any-struct-box-leak-per-widen: the three `any` readers.  Each consumes
         * the tagged value and yields something that cannot alias the payload
         * box, so `b` appearing directly underneath one is a READ, not
         * retention -- which is why the operand is checked confined regardless
         * of this expression's own position:
         *   type-of -- reads the tag; the cstr it returns is a static name from
         *              __tur_any_type_name, never a pointer into the payload.
         *   is?     -- reads the tag; returns bool.
         *   cast    -- unboxes.  For the boxed by-value payload this rule exists
         *              for, that is a DEREF: the result is a copy, so it does
         *              not alias the box.  A carrier payload was never boxed, so
         *              there is nothing this decision could free out from under
         *              it either way. */
        /* any-widen-stored-in-an-adt-field-has-no-owner: a WIDEN neither reads
         * nor retains on its own -- it re-tags its operand, and for a by-value
         * payload copies it into a box.  So it is transparent to this question
         * and the operand is asked in the SAME position the widen occupies: a
         * widen of `b` in an unconfined result position still hands a pointer
         * into b's payload out, while a widen of anything else does not.
         *
         * Left to `default` it deferred to the strict escape walk, whose answer
         * for an unmodelled node is "escapes" -- so a body whose result is a
         * widen (in Saffron, most bodies: an unannotated return is `any`) read
         * as retaining and the frame-box rule never fired. */
        case EX_UNION_INJECT:
            return box_uses_confined(e->as.union_inject_.value, b, confined);
        case EX_ANY_TYPE_OF:
            return box_uses_confined(e->as.any_type_of_.value, b, /*confined=*/true);
        case EX_ANY_IS:
            return box_uses_confined(e->as.any_is_.value, b, /*confined=*/true);
        case EX_ANY_CAST:
            return box_uses_confined(e->as.any_cast_.value, b, /*confined=*/true);
        /* saffron-lang-plan S5: the dynamic operator and the dynamic field read
         * are two more readers of the same kind -- each consumes the tagged
         * value and produces something that CANNOT alias the payload box, so an
         * operand is checked confined whatever this expression's own position
         * is:
         *   dyn-op    -- every helper it lowers to (__tur_dyn_arith / _cmp /
         *                _not / _println / _truthy) returns a FRESH TUR_TAG or a
         *                C scalar.  None returns an operand, so none can hand
         *                back a pointer into one.
         *   dyn-field -- reads a member out of the receiver's box.  A scalar
         *                member is copied into a fresh tag; an aggregate member
         *                is copied into a fresh malloc by dyn_widen_to_any.
         *                Either way the result is a copy, exactly as EX_ANY_CAST
         *                above is a deref.
         *
         * A dynamic CALL is the opposite case and keeps the strict answer: the
         * callee is a value, so there is no body to inspect and no mask to
         * consult -- the same reason EX_CALL refuses an `fn_expr` callee. */
        case EX_DYN_OP:
            for (uint32_t i = 0; i < e->as.dyn_op_.n_args; i++)
                if (!box_uses_confined(e->as.dyn_op_.args[i], b, /*confined=*/true))
                    return false;
            return true;
        case EX_DYN_FIELD:
            return box_uses_confined(e->as.dyn_field_.obj, b, /*confined=*/true);
        case EX_DYN_CALL:
            return false;
        case EX_IF:
            return box_uses_confined(e->as.if_.cond, b, /*discarded=*/true) &&
                   box_uses_confined(e->as.if_.then_, b, confined) &&
                   box_uses_confined(e->as.if_.else_or_null, b, confined);
        case EX_MATCH: {
            if (!box_uses_confined(e->as.match_.scrutinee, b, /*discarded=*/true))
                return false;
            /* Strict mode: the arms' pattern binders alias the scrutinee's
             * spine when the scrutinee is rooted at `b`; track them for the
             * arm, then drop them. */
            bool aliasing = g_bc_strict_handoff &&
                            bc_expr_roots_at_b(e->as.match_.scrutinee, b);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                uint32_t saved_n = g_bc_n_alias;
                if (aliasing) {
                    const MatchPattern *pt = &e->as.match_.arms[i].pattern;
                    for (uint32_t j = 0; j < pt->n_bindings; j++)
                        if (pt->bindings[j] &&
                            bc_kind_can_alias(pt->bindings[j]->type.kind))
                            bc_push_alias(pt->bindings[j]);
                    if (pt->is_var && pt->var_binding)
                        bc_push_alias(pt->var_binding);
                }
                bool ok = box_uses_confined(e->as.match_.arms[i].guard, b, true) &&
                          box_uses_confined(e->as.match_.arms[i].body, b, confined);
                g_bc_n_alias = saved_n;
                if (!ok) return false;
            }
            return true;
        }
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                bool tail = (i + 1 == e->as.do_.n);
                if (!box_uses_confined(e->as.do_.items[i], b, tail ? confined : true))
                    return false;
            }
            return true;
        case EX_LET:
        case EX_LETREC: {
            /* byvalue-recursive-shared-copies-leak (strict mode): a local bound
             * to a call whose result may alias `b` -- `(let [w (id-b zs)] ...)`
             * through a result-alias ^borrow parameter -- is tracked as an
             * alias for the rest of the let instead of refusing the free.  A
             * bare `b` init is NOT: that is a move, and its local becomes the
             * spine's owner. */
            uint32_t saved_n = g_bc_n_alias;
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                const Expr *init = e->as.let_.bindings[i].init;
                const Binding *lb = e->as.let_.bindings[i].binding;
                if (g_bc_strict_handoff && lb && bc_kind_can_alias(lb->type.kind) &&
                    (g_bc_root_unowned || bc_call_result_aliases_b(init, b))) {
                    if (!box_uses_confined(init, b, /*confined=*/true)) {
                        g_bc_n_alias = saved_n;
                        return false;
                    }
                    bc_push_alias(lb);
                    continue;
                }
                /* A binding init's value is retained in the bound local, so a
                 * box-owned pointer flowing there could outlive the box: NOT
                 * confined. */
                if (!box_uses_confined(init, b, false)) {
                    g_bc_n_alias = saved_n;
                    return false;
                }
            }
            bool ok = box_uses_confined(e->as.let_.body, b, confined);
            g_bc_n_alias = saved_n;
            return ok;
        }
        case EX_BUILTIN: {
            /* A print-family builtin (e.g. `println`) consumes -- prints -- its
             * argument and never retains a pointer into it, so it confines its
             * args' results.  Any other builtin operates on scalars; a box-owned
             * pointer never flows into one, and if `b` somehow appears it is
             * checked unconfined (conservative). */
            const char *bn = e->as.builtin.spec ? e->as.builtin.spec->name : NULL;
            bool sink = box_reader_result_void_sink(bn);
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (!box_uses_confined(e->as.builtin.args[i], b, sink))
                    return false;
            return true;
        }
        case EX_CALL: {
            if (e->as.call_.fn_expr) return false;   /* indirect: opaque callee */
            const Binding *fb = e->as.call_.fn_binding;
            const char *nm = (fb && fb->name) ? fb->name->name : NULL;
            bool acc = nm && (strcmp(nm, "ok?") == 0 || strcmp(nm, "err?") == 0 ||
                              strcmp(nm, "ok-val") == 0);
            if (!acc && nm && strcmp(nm, "err-val") == 0 &&
                err_val_result_is_freeable_scalar(e->type.kind))
                acc = true;
            /* value-struct-payload sum drop: under the sum flag the whole
             * audited reader family is an accessor (their results are copies
             * or bools, never a pointer into the arm box). */
            if (!acc && g_esc_allow_sum_accessors && nm && sum_box_reader_name(nm))
                acc = true;
            bool sink = box_reader_result_void_sink(nm);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *a = e->as.call_.args[i];
                /* catch-box-reader-confinement-whitelist: beyond the hardcoded
                 * print family, a USER-DEFINED callee confines this argument
                 * when its body was inferred not to retain that parameter.
                 * Per-argument rather than per-call: a two-parameter logger may
                 * retain one and print the other.  Under the sum flag the
                 * callee's SUM mask answers the same question for a sum-typed
                 * argument -- this is what lets `res-ok?` -> `ok?` chain. */
                bool fb_static = fb && call_dispatch_is_static(e);
                bool arg_sink = sink ||
                    (fb_static && i < 32 && (fb->nonretain_ptr_param_mask & (1u << i))) ||
                    (g_esc_allow_sum_accessors && fb_static && i < 32 &&
                     (fb->nonretain_sum_param_mask & (1u << i)));
                if (a && a->kind == EX_VAR && bc_is_b(a->as.var.binding, b)) {
                    if (acc) continue;             /* scalar result cannot alias */
                    if (arg_sink) continue;        /* printed, not retained */
                    /* byvalue-recursive-shared-copies-leak: a ^borrow callee
                     * that keeps the spine nowhere but its result -- the
                     * result is an alias, so it is safe exactly when THIS
                     * call's result is confined (or, at a let, tracked). */
                    if (g_bc_strict_handoff && fb_static && i < 32 &&
                        (fb->resalias_param_mask & (1u << i))) {
                        if (!confined) return false;
                        continue;
                    }
                    /* Residue 1 (strict mode): a hand-off to a callee not
                     * proven non-retaining is an escape -- it may store the
                     * value, and the spine the caller frees would then be
                     * reachable from wherever it put it. */
                    if (g_bc_strict_handoff) return false;
                    /* sum-payload-stashing-callee-not-dropped: under the SUM
                     * walk a callee that is neither an audited reader nor
                     * proven non-retaining may STORE the box -- `(vec-push!
                     * store o)` in `(defn stash [o : (Option Box)] : int ...)`
                     * -- and the "general reader" rule below only models the
                     * callee's RESULT aliasing the box, not the callee keeping
                     * it.  That stamped `stash` non-retaining, the caller
                     * freed its fresh `(some Box)` after the call, and the
                     * container read it back freed (ASan heap-use-after-free).
                     * The arm box is what this walk's clients free, so such a
                     * hand-off is an escape; the pointer/any walks keep their
                     * reader model. */
                    if (g_esc_allow_sum_accessors) return false;
                    /* general reader: its result aliases box memory -- safe only
                     * if THIS call's result is itself confined. */
                    if (!confined) return false;
                    continue;
                }
                /* A sink/accessor confines its args' results; a general call does
                 * not (it may stash them), so recurse with confined=false there. */
                if (!box_uses_confined(a, b, (arg_sink || acc)))
                    return false;
            }
            return box_uses_confined(e->as.call_.dict_arg, b, false);
        }
        default:
            /* Stores / returns / captures / unmodeled forms: defer to the strict
             * escape walk.  It sees `b` itself (not a bound alias -- those are
             * gated at their let-binding above) and treats any unknown as an
             * escape, so a `true` there correctly denies the free.  Strict mode
             * asks the same of every tracked alias. */
            return bc_unmodelled(e, b);
    }
}

bool localowned_binding_is_confined(const Expr *body, const Binding *b,
                                    bool result_cannot_carry) {
    if (!body || !b) return false;
    g_bc_strict_handoff = true;
    g_bc_root_unowned = b->is_borrow;
    g_bc_n_alias = 0;
    g_bc_alias_overflow = false;
    bool r = box_uses_confined(body, b, /*confined=*/result_cannot_carry);
    if (g_bc_alias_overflow) r = false;
    g_bc_strict_handoff = false;
    g_bc_root_unowned = false;
    g_bc_n_alias = 0;
    g_bc_alias_overflow = false;
    return r;
}

bool localowned_param_is_nonretaining(const Expr *body, const Binding *p) {
    /* confined=true: the caller admits only a non-pointer scalar result, so
     * the body's result position cannot carry a pointer out. */
    return localowned_binding_is_confined(body, p, /*result_cannot_carry=*/true);
}

/* catch-unwind-panic-payload-leaks (Leak 2): true when a caught-box binding `b`
 * whose scope is `body` may be deep-freed at scope exit even though it is read
 * through a reader (not only the scalar-accessor whitelist).  `scope_result`
 * is the type of the box's scope value: the relaxation is sound only if that
 * value cannot itself carry a box-owned pointer out, i.e. it is a non-pointer
 * scalar or nil. */
bool catch_box_binding_reader_confined(const Expr *body, const Binding *b,
                                       TypeKind scope_result) {
    switch (scope_result) {
        case TY_NIL: case TY_INT: case TY_BOOL: case TY_FLOAT:
        case TY_INT64: case TY_UINT64: case TY_INT32: case TY_UINT32:
        case TY_INT16: case TY_UINT16: case TY_INT8: case TY_UINT8:
        case TY_FLOAT64: case TY_FLOAT32:
            break;
        default:
            return false;   /* scope value could carry a box-owned pointer out */
    }
    return box_uses_confined(body, b, /*confined=*/true);
}

/* catch-box-reader-confinement-whitelist: true when `body` provably does not
 * RETAIN a pointer into its pointer-carrying scalar parameter `p` -- every use
 * of `p` is discarded or flows into another non-retaining sink.  This is the
 * inferred replacement for trusting a name list: a user-defined logger whose
 * body only prints its argument now confines exactly like `println` does.
 *
 * `result_cannot_carry` must be true only when the function's own result cannot
 * carry the pointer back out (a non-pointer scalar or nil return); the caller
 * establishes it, mirroring the scope_result gate on
 * catch_box_binding_reader_confined.
 *
 * Same soundness posture as the rest of this family: it only ever greenlights a
 * free, and every unmodeled form falls through to the strict escape walk whose
 * default is "escapes". */
bool ptr_param_is_nonretaining(const Expr *body, const Binding *p,
                               bool result_cannot_carry) {
    if (!body || !p) return false;
    return box_uses_confined(body, p, result_cannot_carry);
}

/* value-struct-payload-sum-monomorph-box-has-no-owner: the same walk for a
 * stdlib Option/Result-typed parameter, with the sum accessor family admitted
 * and callees' sum masks consulted.  Same posture: only ever clears a bit.
 *
 * `result_cannot_carry` plays the role it plays for the pointer walk: the
 * body's result position starts confined only when the function's result is
 * a non-pointer scalar.  An aggregate result (`(Option S)` back out of an
 * `(Option S)` parameter) walks unconfined instead, so a bare `v` in result
 * position -- the identity pass-through, arm box included -- fails, while
 * `result-map`, whose result is rebuilt from `ok-val` / `err-val` reads,
 * still qualifies. */
bool sum_param_is_nonretaining(const Expr *body, const Binding *p,
                               bool result_cannot_carry) {
    if (!body || !p) return false;
    g_esc_allow_sum_accessors = true;
    bool r = box_uses_confined(body, p, /*confined=*/result_cannot_carry);
    g_esc_allow_sum_accessors = false;
    return r;
}

/* Check whether the fall-through point after `e` is unreachable -- i.e. every
 * path through `e` ends in a return/panic.  Unlike expr_is_divergent (which
 * only inspects the last item of a `do`), this treats a `do` as divergent when
 * ANY item diverges, since statements after an unconditional return/panic are
 * unreachable.  Used to decide whether a function body still needs a trailing
 * `return <body-value>`. */
bool expr_tail_diverges(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_RETURN:
        case EX_PANIC:
        case EX_PANIC_WITH:
        case EX_DISCONTINUE:
            return true;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (expr_tail_diverges(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_LET:
            return expr_tail_diverges(e->as.let_.body);
        case EX_IF:
            /* Diverges only if both branches diverge; a missing else falls
             * through. */
            if (!e->as.if_.else_or_null) return false;
            return expr_tail_diverges(e->as.if_.then_) &&
                   expr_tail_diverges(e->as.if_.else_or_null);
        default:
            return false;
    }
}

/* MS1: Check if a program contains any handle expression with a ^multishot case.
 * Used to decide whether to emit the cloneable cont preamble. */
bool expr_has_multishot_handler(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EX_HANDLE: {
            const HandleExpr *h = e->as.handle_.handle;
            if (!h) return false;
            for (uint8_t i = 0; i < h->n_cases; i++) {
                if (h->cases[i].cont_kind == CK_MULTISHOT) return true;
            }
            if (expr_has_multishot_handler(h->body)) return true;
            for (uint8_t i = 0; i < h->n_cases; i++) {
                if (expr_has_multishot_handler(h->cases[i].body)) return true;
            }
            return false;
        }
        case EX_HANDLER_LIT: {
            const HandleExpr *h = e->as.handler_lit_.handle;
            if (!h) return false;
            for (uint8_t i = 0; i < h->n_cases; i++) {
                if (h->cases[i].cont_kind == CK_MULTISHOT) return true;
                if (expr_has_multishot_handler(h->cases[i].body)) return true;
            }
            return false;
        }
        case EX_WITH_HANDLER:
            if (expr_has_multishot_handler(e->as.with_handler_.handler)) return true;
            return expr_has_multishot_handler(e->as.with_handler_.body);
        case EX_COMPOSE_HANDLERS:
            if (expr_has_multishot_handler(e->as.compose_handlers_.h1)) return true;
            return expr_has_multishot_handler(e->as.compose_handlers_.h2);
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                if (expr_has_multishot_handler(e->as.let_.bindings[i].init)) return true;
            }
            return expr_has_multishot_handler(e->as.let_.body);
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (expr_has_multishot_handler(e->as.builtin.args[i])) return true;
            return false;
        case EX_CALL:
            if (e->as.call_.fn_expr && expr_has_multishot_handler(e->as.call_.fn_expr)) return true;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                if (expr_has_multishot_handler(e->as.call_.args[i])) return true;
            return false;
        case EX_MATCH:
            if (expr_has_multishot_handler(e->as.match_.scrutinee)) return true;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                if (expr_has_multishot_handler(e->as.match_.arms[i].guard)) return true;
                if (expr_has_multishot_handler(e->as.match_.arms[i].body)) return true;
            }
            return false;
        case EX_MAKE_STRUCT:
            for (uint32_t i = 0; i < e->as.make_struct_.n_fields; i++)
                if (expr_has_multishot_handler(e->as.make_struct_.field_values[i])) return true;
            return false;
        case EX_WHILE:
            if (expr_has_multishot_handler(e->as.while_.cond)) return true;
            return expr_has_multishot_handler(e->as.while_.body);
        case EX_RETURN:  return expr_has_multishot_handler(e->as.return_.value);
        case EX_SET:     return expr_has_multishot_handler(e->as.set_.value);
        case EX_FN:      return e->as.fn_.fn && expr_has_multishot_handler(e->as.fn_.fn->body);
        case EX_GET_FIELD: return expr_has_multishot_handler(e->as.get_field_.struct_expr);
        case EX_SET_FIELD:
            if (expr_has_multishot_handler(e->as.set_field_.receiver)) return true;
            return expr_has_multishot_handler(e->as.set_field_.value);
        case EX_RESUME:
            if (expr_has_multishot_handler(e->as.resume_.resume->k)) return true;
            return expr_has_multishot_handler(e->as.resume_.resume->value);
        case EX_IF:
            if (expr_has_multishot_handler(e->as.if_.cond)) return true;
            if (expr_has_multishot_handler(e->as.if_.then_)) return true;
            return e->as.if_.else_or_null && expr_has_multishot_handler(e->as.if_.else_or_null);
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++) {
                if (expr_has_multishot_handler(e->as.do_.items[i])) return true;
            }
            return false;
        case EX_FN_DEF:
            return e->as.fn_def_.fn && expr_has_multishot_handler(e->as.fn_def_.fn->body);
        /* cps-backend-n6 cross-function resume: the desugar wraps a reset body in a
         * ^multishot __Shift handler, so descend into reset bodies -- otherwise the
         * synthesized multishot handler is missed and the cloneable-cont preamble
         * is not emitted (undefined tur_cloneable_cont_* at link time). */
        case EX_RESET:
            return expr_has_multishot_handler(e->as.reset_.body);
        case EX_CLONEABLE_RESET:
            return expr_has_multishot_handler(e->as.cloneable_reset_.body);
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++) {
                if (expr_has_multishot_handler(e->as.program.items[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

char *fresh_tmp(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__t%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh frame variable name */
/* defer-frame-chain-must-not-escape: frame -> lexical parent side table.  Frame
 * names are unique per emission (fresh_frame), so no reset is needed and a
 * miss means "no parent". */
static struct { char *frame; char *parent; } *g_frame_parents = NULL;
static uint32_t g_frame_parents_n = 0, g_frame_parents_cap = 0;
void emit_frame_note_parent(const char *frame, const char *parent) {
    if (!frame) return;
    if (g_frame_parents_n == g_frame_parents_cap) {
        g_frame_parents_cap = g_frame_parents_cap ? g_frame_parents_cap * 2 : 64;
        g_frame_parents = (void *)realloc(g_frame_parents,
            g_frame_parents_cap * sizeof(*g_frame_parents));
        if (!g_frame_parents) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    g_frame_parents[g_frame_parents_n].frame = strdup(frame);
    g_frame_parents[g_frame_parents_n].parent = parent ? strdup(parent) : NULL;
    g_frame_parents_n++;
}
const char *emit_frame_parent(const char *frame) {
    if (!frame) return NULL;
    for (uint32_t i = g_frame_parents_n; i-- > 0; )
        if (strcmp(g_frame_parents[i].frame, frame) == 0)
            return g_frame_parents[i].parent;
    return NULL;
}
char *fresh_frame(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__frame_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh defer thunk function name */
char *fresh_defer_thunk(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__defer_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Generate a fresh defer env struct name */
char *fresh_defer_env(EmitCtx *ctx) {
    char *p = (char *)malloc(24);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(p, 24, "__defer_env_%d", ctx->tmp_n++);
    return p;
}

/* Phase 4 v1: Register a defer thunk to be emitted at file scope */
void register_defer_thunk(EmitCtx *ctx, const char *name, const Expr *body, 
                                  Binding **captures, uint8_t n_captures, 
                                  const char *env_name) {
    DeferThunk *thunk = (DeferThunk *)malloc(sizeof(DeferThunk));
    if (!thunk) { fprintf(stderr, "tur: oom\n"); abort(); }
    thunk->name = (char *)name;
    thunk->body = (Expr *)body;  /* Cast away const for storage */
    thunk->captures = captures;
    thunk->n_captures = n_captures;
    thunk->env_name = env_name ? strdup(env_name) : NULL;
    thunk->next = ctx->pending_defer_thunks;
    ctx->pending_defer_thunks = thunk;
}

/* Phase 4 v1: Emit all registered defer thunks to the output buffer */
/* `decls` gets each thunk's env struct and prototype, `out` its definition.
 * The definitions go after the program's file-scope globals, which a defer
 * body may read or `set!` -- before them, `(defer (set! g ...))` named a
 * global not yet declared (invalid C) -- and the prototypes before the
 * bodies that push the thunks. */
void emit_pending_defer_thunks(EmitCtx *ctx, Buf *decls, Buf *out) {
    /* First pass: emit env struct definitions for thunks with captures */
    DeferThunk *thunk = ctx->pending_defer_thunks;
    while (thunk) {
        if (thunk->env_name) {
            /* Emit env struct type definition */
            buf_printf(decls, "struct %s {", thunk->env_name);
            for (uint8_t i = 0; i < thunk->n_captures; i++) {
                if (i > 0) buf_puts(decls, "; ");
                Binding *captured = thunk->captures[i];
                char *field = raw_name_for_binding(captured);
                buf_printf(decls, "%s %s",
                           type_c_name(captured->type), field);
                free(field);
            }
            buf_puts(decls, "; };\n\n");
        }
        buf_printf(decls, "static void %s(void *__env);\n", thunk->name);
        thunk = thunk->next;
    }
    
    /* Second pass: emit thunk functions */
    thunk = ctx->pending_defer_thunks;
    while (thunk) {
        if (thunk->env_name) {
            /* Thunk with captures - cast void* to env struct type */
            buf_printf(out, "static void %s(void *__env) {\n", thunk->name);
            buf_printf(out, "    struct %s *__e = (struct %s *)__env;\n",
                       thunk->env_name, thunk->env_name);
            
            /* Set up env access context for name_for_binding */
            const char *saved_env_var_name = ctx->env_var_name;
            Binding **saved_defer_captures = ctx->defer_captures;
            uint8_t saved_n_defer_captures = ctx->n_defer_captures;
            
            ctx->env_var_name = "__e";
            ctx->defer_captures = thunk->captures;
            ctx->n_defer_captures = thunk->n_captures;
            
            /* Emit the body with env access */
            int saved_indent = ctx->indent;
            ctx->indent = 4;
            emit_stmt(ctx, out, thunk->body);
            ctx->indent = saved_indent;
            
            ctx->env_var_name = saved_env_var_name;
            ctx->defer_captures = saved_defer_captures;
            ctx->n_defer_captures = saved_n_defer_captures;
        } else {
            /* Thunk without captures */
            buf_printf(out, "static void %s(void *__env) {\n", thunk->name);
            int saved_indent = ctx->indent;
            ctx->indent = 4;
            emit_stmt(ctx, out, thunk->body);
            ctx->indent = saved_indent;
        }
        buf_puts(out, "}\n\n");
        thunk = thunk->next;
    }
    
    /* Free the list */
    while (ctx->pending_defer_thunks) {
        DeferThunk *tmp = ctx->pending_defer_thunks;
        ctx->pending_defer_thunks = tmp->next;
        free(tmp->name);
        free(tmp->env_name);  /* May be NULL */
        free(tmp);
    }
}








/* DV2: Return a malloc'd C identifier for a dynamic var name.
 * Strips leading/trailing '*' (earmuffs), then applies the injective mangler.
 * E.g. "*log-level*" -> "log_hydlevel", "*log_level*" -> "log_unlevel".
 * Caller frees. */
char *mangle_dynvar_name(const char *name) {
    const char *start = name;
    size_t mlen = strlen(name);
    if (mlen > 0 && start[0] == '*') { start++; mlen--; }
    if (mlen > 0 && start[mlen - 1] == '*') { mlen--; }
    size_t cap = tur_mangle_bound(mlen) + 1;
    char *p = (char *)malloc(cap);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    size_t k = 0;
    tur_mangle_append(p, &k, start, mlen);
    p[k] = '\0';
    return p;
}

/* Mangle a Turmeric struct field name to a valid C identifier.
 *
 * Struct fields (and dynvars) deliberately keep the LEGACY '-'/'/' -> '_' fold
 * rather than the injective scheme used for linker-visible global symbols.
 * Two reasons (see docs/guides/name-mangling-guide.md):
 *   1. A field is referenced inside inline-C bodies by this stable spelling
 *      (`opt->is_some` for a field `is-some`), exactly like a function-local.
 *   2. Field names routinely coincide with parameter names (e.g. a Zipper
 *      field `left-len` and a `zipper-new` parameter `left-len`); since
 *      parameters are also legacy-folded, an injective field name would desync
 *      the two for the same inline-C token. Fields are struct-scoped and never
 *      cause linker collisions, so injectivity buys nothing here.
 * Caller frees. */
char *mangle_field_name(const char *name) {
    size_t len = strlen(name);
    /* c-keyword-function-names-not-mangled: a field (or struct/ctor) named
     * after a C reserved word emits `int64_t int;` / `struct enum { ... }`.
     * Guard it exactly as raw_name_for_binding guards a colliding global. This
     * is the single chokepoint for declaration and every access site, so both
     * move together. */
    size_t pre = tur_name_is_c_keyword(name, len) ? TUR_NAME_GUARD_PREFIX_LEN : 0;
    char *p = (char *)malloc(pre + len + 1);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    if (pre) memcpy(p, TUR_NAME_GUARD_PREFIX, pre);
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            p[pre + i] = c;
        } else {
            p[pre + i] = '_';
        }
    }
    p[pre + len] = '\0';
    return p;
}

/* separator-fold-collides-emitted-c-names: the C spelling of an ADT or
 * constructor NAME.  These names are joined into larger symbols with `_`
 * (`ctor_<Adt>_<Ctor>`) and `__` (`tur_adt_<Adt>__<arg>__<arg>`), so the
 * per-name fold must never PRODUCE the joiner: the legacy field fold above
 * maps `-`, `/` and a literal `_` all to `_`, which made `a-b`+`c` and
 * `a`+`b-c` one `ctor_a_b_c`, and a user ADT named `Foo__int` the same C type
 * as the `(Foo int)` monomorph.  This uses the injective scheme from
 * mangle.h (`-` -> `_hy`, `_` -> `_un`, `/` -> `_sl`, sigils by mnemonic), so a
 * single `_` in the output always introduces an escape and `_` / `__` are
 * structural only.  Every site that spells an ADT / constructor name --
 * typedefs, ctor symbols, the `as.<Ctor>._N` member path, drop glue, the
 * bare-ctor alias -- goes through here or through types.c's
 * append_c_ident_mangled, which must agree byte for byte.  FIELD names keep
 * mangle_field_name: inline-C reads them by that spelling.  A name that is
 * already a plain identifier (letters, digits, no `_`) spells the same under
 * both, which is every ADT and ctor name in the tree.  Caller frees. */
char *mangle_adt_name(const char *name) {
    size_t len = strlen(name);
    size_t pre = tur_name_is_c_keyword(name, len) ? TUR_NAME_GUARD_PREFIX_LEN : 0;
    char *p = (char *)malloc(pre + tur_mangle_bound(len) + 1);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    size_t k = 0;
    if (pre) { memcpy(p, TUR_NAME_GUARD_PREFIX, pre); k = pre; }
    tur_mangle_append(p, &k, name, len);
    p[k] = '\0';
    return p;
}

/* duplicate-ctor-names-collide-in-emitted-c: a program-wide census of which
 * constructor NAMES are owned by more than one ADT.
 *
 * The emitted C symbol is `ctor_<Adt>_<Ctor>` so two ADTs sharing a constructor
 * name no longer collide.  But that symbol has a second life: hand-written
 * inline C calls constructors by their emitted name, and stdlib documents it
 * (`stdlib/either.tur`: "Construct with ctor_Left(v) / ctor_Right(v)").  So a
 * constructor name owned by exactly ONE ADT also gets a bare-name alias, and
 * that inline C keeps working untouched -- in this tree and out of it.
 *
 * When a name IS owned by two ADTs there is no correct bare alias, so none is
 * emitted: inline C naming it then fails at cc with an implicit declaration
 * pointing at the ambiguous constructor, instead of silently binding to
 * whichever ADT was emitted first.  Fail-closed is deliberate -- a missing
 * alias is a loud compile error, a wrong one is a silent wrong answer.
 *
 * Ambiguity is keyed on the ADT NAME, not the AdtDef pointer: re-elaboration
 * and module reloads legitimately produce two AdtDefs for one ADT, and those
 * spell the SAME qualified symbol, so they are not a conflict.
 *
 * Fed from elab_register_adt_def, the single chokepoint every ADT passes
 * through, so the census is complete before any emission starts. */
/* Snapshotted at the END of elaboration, not during it.  Two reasons, both
 * found the hard way:
 *
 *  - An ADT is REGISTERED when its AdtDef is created, which is before its
 *    constructors are attached, so reading `def->ctors` at registration
 *    recorded nothing at all.
 *  - Holding the AdtDef pointers instead and reading them at emit time is a
 *    use-after-poison: a procedural macro runs a nested elaboration whose arena
 *    is released, and the census then walks freed defs (ASan caught this on the
 *    eight macro-procedural fixtures).
 *
 * So the census owns COPIES of the (ADT name, constructor name) pairs, taken
 * at the one moment both are live and complete.  A nested elaboration snapshots
 * too, but the outer one returns last, so the final census is the real
 * program's. */
typedef struct {
    char *adt_name;
    char *ctor_name;
} CtorCensusRow;

static CtorCensusRow *g_ctor_census     = NULL;
static uint32_t       g_n_ctor_census   = 0;
static uint32_t       g_cap_ctor_census = 0;

void ctor_census_reset(void) {
    for (uint32_t i = 0; i < g_n_ctor_census; i++) {
        free(g_ctor_census[i].adt_name);
        free(g_ctor_census[i].ctor_name);
    }
    free(g_ctor_census);
    g_ctor_census = NULL;
    g_n_ctor_census = 0;
    g_cap_ctor_census = 0;
}

/* Both names are stored MANGLED.  The alias the census gates expands to
 * `ctor_<mangled adt>_<mangled ctor>` and is guarded on `<mangled ctor>`, so
 * the mangled spelling is the only one that answers the question being asked.
 * Storing the raw name made the uniqueness test and its guard disagree about
 * what "the same name" means: `b-c` and `b_c` in two different ADTs read as two
 * distinct unique names, both emitted an alias, and the second `#define` was
 * dropped by its own `#ifndef` -- so `ctor_b_c` in inline C silently reached
 * the FIRST ADT's constructor.  With both ADTs on the int64 carrier, C's type
 * system could not see it either: no turmeric error, no cc warning, no ASan
 * report, just the wrong constructor.  See
 * docs/reported/separator-fold-collides-emitted-c-names.md. */
static void ctor_census_push(const char *adt_name, const char *ctor_name) {
    if (g_n_ctor_census >= g_cap_ctor_census) {
        uint32_t nc = g_cap_ctor_census ? g_cap_ctor_census * 2 : 64;
        CtorCensusRow *nr = (CtorCensusRow *)realloc(g_ctor_census,
                                                    nc * sizeof(CtorCensusRow));
        if (!nr) { fprintf(stderr, "tur: oom\n"); abort(); }
        g_ctor_census = nr;
        g_cap_ctor_census = nc;
    }
    char *a = mangle_adt_name(adt_name), *c = mangle_adt_name(ctor_name);
    if (!a || !c) { fprintf(stderr, "tur: oom\n"); abort(); }
    g_ctor_census[g_n_ctor_census].adt_name  = a;
    g_ctor_census[g_n_ctor_census].ctor_name = c;
    g_n_ctor_census++;
}

/* Replace the census with the constructor names of `defs`.  Called from
 * elaborate_program_session's success return, where every def is live and its
 * constructors are attached. */
void ctor_census_snapshot(struct AdtDef *const *defs, uint32_t n_defs) {
    ctor_census_reset();
    if (!defs) return;
    for (uint32_t i = 0; i < n_defs; i++) {
        const AdtDef *d = defs[i];
        if (!d || !d->name || !d->ctors) continue;
        for (uint32_t ci = 0; ci < d->n_ctors; ci++) {
            const CtorDef *c = d->ctors[ci];
            if (!c || !c->name) continue;
            ctor_census_push(d->name, c->name);
        }
    }
}

/* True when every constructor whose MANGLED name is `mangled_ctor_name` belongs
 * to one ADT -- so the bare-name alias for it has exactly one meaning.
 *
 * `mangled_ctor_name` must already be mangled: it is compared against the
 * census's mangled rows, and it is the same string the alias is guarded on.
 * Two constructors that differ only by a separator (`b-c`, `b_c`) mangle alike
 * and are correctly a conflict here, because they would fight over one macro.
 *
 * A name the census never saw answers false -- fail closed, because a missing
 * alias is a loud compile error and a wrong one is a silent wrong answer. */
bool ctor_base_name_is_unique(const char *mangled_ctor_name) {
    if (!mangled_ctor_name) return false;
    const char *owner = NULL;
    for (uint32_t i = 0; i < g_n_ctor_census; i++) {
        if (strcmp(g_ctor_census[i].ctor_name, mangled_ctor_name) != 0) continue;
        /* Compared on the mangled ADT name, not identity: re-elaboration and
         * module reloads legitimately yield two entries for one ADT, and both
         * spell the SAME qualified symbol, so they are not a conflict.  Two
         * ADTs whose names differ but mangle alike are likewise not a conflict
         * for the ALIAS -- they already collide at the qualified symbol, which
         * is the other half of the same report. */
        if (!owner) owner = g_ctor_census[i].adt_name;
        else if (strcmp(owner, g_ctor_census[i].adt_name) != 0) return false;
    }
    return owner != NULL;
}

/* Emit the bare-name compatibility alias for `ctor`, when its name is
 * unambiguous.  A macro rather than a forwarder function: it works for any
 * arity and return type, costs nothing, and needs no signature duplication.
 * Header-guarded so a re-emitted ADT does not redefine it. */
void emit_ctor_bare_alias(Buf *out, const AdtDef *def, const CtorDef *ctor) {
    if (!out || !def || !ctor || !ctor->name) return;
    /* Ask the census about the same string the guard below uses -- the MANGLED
     * name.  These two disagreeing is what made the alias bind silently to the
     * wrong ADT; keeping the query and the guard on one spelling is the fix. */
    char *bare = mangle_adt_name(ctor->name);
    if (!ctor_base_name_is_unique(bare)) { free(bare); return; }
    char *qual = mangle_ctor_symbol(def, ctor->name);
    if (strcmp(bare, qual) != 0) {
        buf_printf(out, "#ifndef TUR_CTORALIAS_%s\n", bare);
        buf_printf(out, "#define TUR_CTORALIAS_%s\n", bare);
        buf_printf(out, "#define ctor_%s ctor_%s\n", bare, qual);
        buf_printf(out, "#endif\n");
    }
    free(bare);
    free(qual);
}

/* duplicate-ctor-names-collide-in-emitted-c: the base token of a constructor's
 * emitted C FUNCTION symbol -- `<Adt>_<Ctor>`, so the full symbol is
 * `ctor_<Adt>_<Ctor>` (plus a monomorph's type-arg suffix).  Caller frees.
 *
 * The owning ADT has to be part of the symbol.  Without it two ADTs that share
 * a constructor name emit one C function twice (`redefinition of 'ctor_Mk'`) --
 * elaboration handles the shadowing correctly, so this is purely the emitted C
 * merging two distinct constructors.  SR2b widened the trigger from "two of
 * your own ADTs happen to collide" to "your ADT names a constructor Some, None,
 * Ok or Err", because stdlib Option/Result are sums now and their constructors
 * are always in the program.
 *
 * This is the FUNCTION symbol only.  A constructor's union MEMBER name inside
 * its own ADT's struct (`as.<Ctor>._N`) is already scoped by that struct and
 * stays bare -- see adt_field_member_path just below.
 *
 * Residual, and shared with the pre-existing type-arg suffix convention: every
 * non-alphanumeric character mangles to '_', so an ADT `a-b` with constructor
 * `c` and an ADT `a` with constructor `b-c` both spell `ctor_a_b_c`.  That
 * needs two ADTs whose names differ by exactly where one separator falls; the
 * bug being fixed here needed only a shared constructor name, which is
 * ordinary. */
char *mangle_ctor_symbol(const AdtDef *adt, const char *ctor_name) {
    char *mctor = mangle_adt_name(ctor_name);
    if (!adt || !adt->name) return mctor;   /* nothing to namespace against */
    char *madt = mangle_adt_name(adt->name);
    Buf b; buf_init(&b);
    buf_printf(&b, "%s_%s", madt, mctor);
    buf_putc(&b, '\0');
    char *r = strdup(b.data);
    if (!r) { fprintf(stderr, "tur: oom\n"); abort(); }
    buf_free(&b);
    free(madt);
    free(mctor);
    return r;
}

/* CONV-S1 seam 4: build the C member-access path (without a leading '.'/'->')
 * for constructor field `fi`.  A flat, named-layout ADT (adt_uses_named_layout)
 * is accessed by its mangled field name (`value`, `max_age`); every other ADT
 * keeps the positional tagged-union path (`as.<Ctor>._N`).  Caller frees.
 * Single source of truth for the typedef/ctor/field-access/match/drop-glue
 * emit sites so they never disagree on the layout. */
char *adt_field_member_path(const AdtDef *def, const CtorDef *ctor, uint32_t fi) {
    if (adt_uses_named_layout(def))
        return mangle_field_name(ctor->fields[fi].name);
    char *mctor = mangle_adt_name(ctor->name);
    Buf b; buf_init(&b);
    buf_printf(&b, "as.%s._%u", mctor, fi);
    buf_putc(&b, '\0');
    char *r = strdup(b.data);
    buf_free(&b);
    free(mctor);
    return r;
}

/* Return a sanitized C identifier for a Binding, without the ID suffix.
 * Used for function names and parameters. Caller frees.
 *
 * Phase M3: For module-level bindings (defining_module_name != NULL), the
 * C name is prefixed with the mangled module name: geom/vector → geom__vector__,
 * so binding `add2` in module `geom/vector` → `geom__vector__add2`.
 * Phase M6: If b->c_export_name is set, use it directly (bypasses mangling). */
char *raw_name_for_binding(const Binding *b) {
    if (b->c_export_name) {
        return strdup(b->c_export_name);
    }
    /* Compiler-synthesized names with the reserved "__" prefix that are ALREADY
     * pure C identifiers (anonymous lambdas `__fn_N`, instance dicts `__inst_*`)
     * are emitted verbatim at their use sites, so the definition must match:
     * re-encoding their literal '_' as "_un" would desync def from use
     * (`__fn_5` -> `_un_unfn_un5`). Names that merely START with "__" but still
     * contain kebab/sigil bytes (e.g. `__tur-q-is-err?`) are NOT pure C
     * identifiers; they fall through to the injective mangler, which is applied
     * consistently at def and use. Users cannot define "__"-prefixed names. */
    /* extern-c bindings name a real C symbol via the LEGACY fold (matches the
     * prototype emitted with mangle_field_name); never injectively mangled. */
    if (b->is_extern_c) {
        return mangle_field_name(b->name->name);
    }
    /* Build module prefix if this binding belongs to a named module.
     * Exception: `main` is always the C entry point, never prefixed. */
    char mod_prefix[512];
    size_t mod_prefix_len = 0;
    bool is_main_binding = (b->name->len == 4 &&
                             memcmp(b->name->name, "main", 4) == 0);
    /* Phase M7: Only globals get module-prefixed C names. Function parameters
     * and locals are scoped to their function, so they don't collide across
     * modules — and inline-C bodies reference them by their source names. */
    if (b->defining_module_name != NULL && !is_main_binding && b->is_global) {
        const char *mn = b->defining_module_name->name;
        size_t mn_len = b->defining_module_name->len;
        size_t j = 0;
        /* Each '/'-separated component is mangled through the shared injective
         * scheme; '/' itself becomes the "__" structural separator (which data
         * can never produce, since a literal '_' encodes as "_un"). A trailing
         * "__" terminates the prefix. Reserve 6 bytes of slack: up to a 4-byte
         * "_xHH" escape plus the trailing "__". */
        for (size_t i = 0; i < mn_len && j + 6 < sizeof(mod_prefix); i++) {
            char c = mn[i];
            if (c == '/') {
                mod_prefix[j++] = '_';
                mod_prefix[j++] = '_';
            } else {
                tur_mangle_append(mod_prefix, &j, &mn[i], 1);
            }
        }
        mod_prefix[j++] = '_';
        mod_prefix[j++] = '_';
        mod_prefix[j]   = '\0';
        mod_prefix_len  = j;
    }

    /* +8 slack reserves room for the `tur_u_` libc-collision guard prefix
     * (6 bytes) plus the NUL, applied to a bare global below. */
    size_t total = mod_prefix_len + tur_mangle_bound(b->name->len) + 8;
    char *p = (char *)malloc(total);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    size_t k = 0;
    if (mod_prefix_len > 0) {
        memcpy(p, mod_prefix, mod_prefix_len);
        k = mod_prefix_len;
    }
    /* Name component:
     *  - Compiler-synthesized "__"-prefixed names that are already pure C
     *    identifiers (anonymous lambdas `__fn_N`, internal stdlib helpers like
     *    `__fiber_set_cancelled`) are emitted VERBATIM -- their use sites
     *    reference them by that exact spelling. The module prefix above still
     *    applies, so two module-private `__h` helpers stay distinct
     *    (`alpha____h` vs `beta____h`).
     *  - Globals are linker-visible and injective (the foo-bar/foo_bar
     *    de-collision). Locals/params are block-scoped, never collide, and are
     *    referenced by inline-C via the legacy `-`->`_` spelling. */
    if (tur_name_is_c_identifier(b->name->name, b->name->len) &&
        b->name->len >= 2 && b->name->name[0] == '_' && b->name->name[1] == '_') {
        memcpy(p + k, b->name->name, b->name->len);
        k += b->name->len;
    } else if (b->is_global) {
        /* codegen-user-defn-collides-with-libc-pipe2: a bare (non-module)
         * top-level global whose spelling is a libc/POSIX symbol the system
         * headers declare would emit `static int64_t read(...)` and conflict
         * with e.g. <unistd.h>'s `read` ("static declaration follows non-static
         * declaration"). Prefix such names with `tur_u_` so the user function
         * gets its own C symbol. Module-qualified globals already carry a
         * distinguishing prefix (mod_prefix_len > 0, so `geom__read` never
         * matches), and extern-c bindings (handled above) intentionally keep
         * the libc spelling to name the real symbol. Applied at both definition
         * and every use through this single chokepoint, so all sites agree;
         * `main` is never remapped. */
        if (mod_prefix_len == 0 && !is_main_binding &&
            tur_name_collides_libc(b->name->name, b->name->len)) {
            memcpy(p + k, TUR_NAME_GUARD_PREFIX, TUR_NAME_GUARD_PREFIX_LEN);
            k += TUR_NAME_GUARD_PREFIX_LEN;
        }
        /* c-keyword-function-names-not-mangled: `(defn double ...)` would emit
         * `static int64_t double(int64_t);`. Unlike the libc guard this applies
         * only when the name reaches C unqualified -- a module-prefixed global
         * is `geom__double`, which is not a keyword. */
        else if (mod_prefix_len == 0 &&
                 tur_name_is_c_keyword(b->name->name, b->name->len)) {
            memcpy(p + k, TUR_NAME_GUARD_PREFIX, TUR_NAME_GUARD_PREFIX_LEN);
            k += TUR_NAME_GUARD_PREFIX_LEN;
        }
        tur_mangle_append(p, &k, b->name->name, b->name->len);
    } else {
        /* Function-locals and parameters. A keyword here lands as
         * `f(int64_t double)` / `int64_t return;`, so it needs the same guard --
         * and an inline-C body could never have referenced the raw spelling
         * anyway, since it would not have parsed. */
        if (tur_name_is_c_keyword(b->name->name, b->name->len)) {
            memcpy(p + k, TUR_NAME_GUARD_PREFIX, TUR_NAME_GUARD_PREFIX_LEN);
            k += TUR_NAME_GUARD_PREFIX_LEN;
        }
        tur_mangle_legacy_append(p, &k, b->name->name, b->name->len);
    }
    p[k] = '\0';
    return p;
}

/* GHE (constrained-generic-instance-dispatch): the single-component type suffix
 * used in instance-method mangled names (__inst_<Class>_<method>_<component>).
 * Mirrors the type_suffix switch in emit_stmt.c's EX_INSTANCE_DEF.  Returns NULL
 * for types we do not re-resolve (the caller then keeps the baked representative
 * callee, which is the carrier-correct TY_INT instance). */
static const char *emit_inst_suffix_component(TypeKind k) {
    switch (k) {
        case TY_INT:     return "int";
        case TY_BOOL:    return "bool";
        case TY_CSTR:    return "cstr";
        case TY_NIL:     return "nil";
        case TY_INT8:    return "int8";
        case TY_INT16:   return "int16";
        case TY_INT32:   return "int32";
        case TY_UINT8:   return "uint8";
        case TY_UINT16:  return "uint16";
        case TY_UINT32:  return "uint32";
        case TY_UINT64:  return "uint64";
        case TY_FLOAT:   return "float";
        case TY_FLOAT32: return "float32";
        case TY_FLOAT64: return "float64";
        case TY_SYM:     return "Sym";
        default:         return NULL;
    }
}

/* Gap H (bounded-storageops-wrapper-heterogeneous-monomorphisation-gap):
 * resolve the *authoritative* emitted instance-method symbol for a concrete
 * dispatch type, by locating the actual instance and reading its method impl's
 * FnDef binding name.
 *
 * Single-component reconstruction (`__inst_<Class>_<method>_<head>`) is correct
 * only for instances keyed on one ground type.  A parametric / multi-parameter
 * instance head such as `StorageOps [(Dense Pos) Pos]` is emitted under the full
 * type-arg suffix (`_Dense_PosPos`), so reconstructing from the receiver's head
 * alone (`_Dense`) calls an undefined symbol.  The instance's method FnDef
 * binding name is the same spelling EX_INSTANCE_DEF wires into the dict
 * singleton, so returning it keeps the wrapper's call site and the emitted impl
 * in lockstep regardless of how many type args the instance head carries.
 *
 * `resolved` is the concrete dispatch type (already resolved from the spec's
 * bindings); `method_field_name` is the dict's mangled method field.  Returns a
 * malloc'd symbol name, or NULL when no concrete instance of `tc` matches
 * `resolved` (the caller then falls back to single-component reconstruction). */
/* Gap H follow-up (single-param + associated-type bounded wrapper):
 * does the concrete dispatch type `concrete` instantiate the instance-head
 * type-arg `pattern`?  A strict `type_eq` is correct for a ground instance head
 * (`StorageOps [(Dense Pos) Pos]`), but too strict for a *parametric* head whose
 * element is a free type variable -- `(definstance StorageOps [(Dense A)] ...)`
 * has `type_args[0] == TY_APP(Dense, TYVAR A)`, which `type_eq` never equates
 * with the concrete receiver `TY_APP(Dense, Pos)`.  Treat a TY_TYVAR in the
 * pattern as a wildcard so `(Dense A)` matches `(Dense Pos)` (head constructors
 * still compared by identity), letting the authoritative-symbol lookup succeed
 * instead of falling through to single-component reconstruction (`_Dense`),
 * which desyncs from the instance's emitted suffix (`_Dense__ltstruct_gt`). */
static bool emit_inst_head_matches(Type pattern, Type concrete) {
    if (pattern.kind == TY_TYVAR) return true; /* free element: matches anything */
    if (type_eq(pattern, concrete)) return true;
    if (pattern.kind == TY_APP && concrete.kind == TY_APP) {
        bool fn_ok = (pattern.as.app.fn && concrete.as.app.fn)
                         ? emit_inst_head_matches(*pattern.as.app.fn,
                                                  *concrete.as.app.fn)
                         : pattern.as.app.fn == concrete.as.app.fn;
        bool arg_ok = (pattern.as.app.arg && concrete.as.app.arg)
                          ? emit_inst_head_matches(*pattern.as.app.arg,
                                                   *concrete.as.app.arg)
                          : pattern.as.app.arg == concrete.as.app.arg;
        return fn_ok && arg_ok;
    }
    /* G2 (HKT instance head): an instance written over a bare type constructor
     * (`definstance Enc [Cons]`, head `Cons` with type params) matches any
     * concrete application `(Cons X)`.  This mirrors the `__inst_<Class>_<method>
     * __<Head>` name the suffix-reconstruction fallback already builds, so the
     * FnDef lookup agrees with the name resolver instead of falling through to
     * reconstruction.  Without it a nested dispatch on a parametric-container
     * element cannot find the inner instance's FnDef to mint its by-value spec. */
    /* CONV-S1 seam 4: the ADT mirror of the G2 head match.  An instance written
     * over a bare type constructor whose head is a record ADT (`definstance Dec
     * [Option]`, head `Option`) -- including a `defstruct` lowered to a record
     * defadt -- matches any concrete application `(Option int)`.  Without this a
     * nested/return dispatch that recovers an APPLIED element type (`build`'s
     * `(dec i)` re-resolving to `(Option int)`) cannot find the `Dec [Option]`
     * instance FnDef, falls back to the int-carrier representative, and mints an
     * ill-typed `__inst_Dec_dec_int__spec` returning the wrong element. */
    if (pattern.kind == TY_ADT && pattern.as.adt_.def &&
        pattern.as.adt_.def->n_type_params > 0 && concrete.kind == TY_APP) {
        Type head = concrete;
        while (head.kind == TY_APP && head.as.app.fn) head = *head.as.app.fn;
        if (head.kind == TY_ADT &&
            head.as.adt_.def == pattern.as.adt_.def) {
            return true;
        }
    }
    return false;
}

/* nested-construct-byvalue: locate the concrete instance-method FnDef whose head
 * matches `resolved` (shared by the name lookup and the scan-time liveness mark). */
FnDef *emit_concrete_inst_method_fndef(EmitCtx *ctx, const TypeClass *tc,
                                       Type resolved,
                                       const char *method_field_name) {
    if (!ctx || !ctx->program_root || !tc || !method_field_name ||
        method_field_name[0] == '\0') {
        return NULL;
    }
    uint32_t n_items = 0;
    const Expr **items = flatten_program_items(ctx->program_root, &n_items);
    if (!items) return NULL;
    FnDef *result = NULL;
    /* Two passes: prefer a match at the primary (receiver) type-arg position --
     * the conventional argument-dispatch slot -- before falling back to a match
     * at any position, which covers a return-position-only dispatch. */
    for (int pass = 0; pass < 2 && !result; pass++) {
        for (uint32_t i = 0; i < n_items && !result; i++) {
            if (items[i]->kind != EX_INSTANCE_DEF) continue;
            TypeClassInstance *inst = items[i]->as.instance_def_.instance;
            if (!inst || inst->typeclass != tc) continue;
            bool matched = false;
            if (pass == 0) {
                matched = inst->n_type_args >= 1 &&
                          emit_inst_head_matches(inst->type_args[0], resolved);
            } else {
                for (uint8_t j = 0; j < inst->n_type_args; j++) {
                    if (emit_inst_head_matches(inst->type_args[j], resolved)) {
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) continue;
            for (uint8_t mi = 0;
                 mi < tc->n_methods && mi < inst->n_method_impls; mi++) {
                if (!tc->methods[mi].name) continue;
                char field[64];
                tur_mangle_ident(tc->methods[mi].name->name, field, sizeof(field));
                if (strcmp(field, method_field_name) != 0) continue;
                result = inst->method_impls[mi];
                break;
            }
        }
    }
    free((void *)items);
    return result;
}

static char *emit_concrete_inst_method_name(EmitCtx *ctx, const TypeClass *tc,
                                            Type resolved,
                                            const char *method_field_name) {
    FnDef *impl = emit_concrete_inst_method_fndef(ctx, tc, resolved,
                                                  method_field_name);
    if (impl && impl->binding && impl->binding->name) {
        char *result = strdup(impl->binding->name->name);
        if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
        return result;
    }
    return NULL;
}

/* nested-construct-byvalue: structurally match a class-method's declared result
 * pattern (e.g. `(Result a cstr)`, carrying the class type var `a`) against the
 * concrete/ascribed call result (`(Result A cstr)`), returning the subtype at
 * the position the class var occupies.  Lets a RETURN-dispatch method whose
 * result is a TY_APP *containing* the class var (not a bare tyvar) recover its
 * dispatch type from the ascription, then resolve it through the active spec. */
static bool emit_pattern_extract_classvar(const Type *pattern, const Type *concrete,
                                          const char *varname, Type *out) {
    if (!pattern || !concrete || !varname) return false;
    if (pattern->kind == TY_TYVAR && pattern->as.tyvar_.name &&
        strcmp(pattern->as.tyvar_.name, varname) == 0) {
        *out = *concrete;
        return true;
    }
    if (pattern->kind == TY_APP && concrete->kind == TY_APP) {
        if (emit_pattern_extract_classvar(pattern->as.app.fn, concrete->as.app.fn,
                                          varname, out))
            return true;
        if (emit_pattern_extract_classvar(pattern->as.app.arg, concrete->as.app.arg,
                                          varname, out))
            return true;
    }
    return false;
}

/* GHE: re-resolve a typeclass-method call inside a monomorphized constrained
 * generic.  When the call carries a dict_arg annotation (so it is a typeclass
 * method call) and its receiver argument's type is a type variable that the
 * current ABI specialization binds to a concrete scalar, return the correct
 * __inst_<Class>_<sanitized-method>_<component> name.  Returns NULL when no
 * re-resolution applies (the caller keeps the baked representative callee, which
 * is the TY_INT carrier instance -- correct for the int / base-clone case). */
/* nested-construct-byvalue: shared dispatch-type resolution for a return- or
 * argument-dispatched typeclass method call inside an active ABI spec.  Returns
 * the concrete dispatch type (`out_resolved`) and the call's dict (`out_dict`),
 * or false when no re-resolution applies.  Used by both the emit-time name
 * rewrite (emit_reresolve_method_call) and the scan-time liveness mark
 * (emit_reresolve_method_fndef), so the two never disagree about which concrete
 * instance a constrained-instance body dispatches to. */
/* R2 (carrier-crossing-recovery-routing-plan): shared first-stage dispatch-tyvar
 * identification.  A typeclass-method call carries its dispatch type variable in
 * one of three places, checked in priority order:
 *   1. an explicit ascription-to-tyvar on the receiver -- `(enc (:: e A))`, the
 *      documented carrier-helper element-read idiom (captured BEFORE stripping
 *      ascriptions, so the inner int-carrier type is not read instead);
 *   2. the bare receiver (arg 0) once ascriptions are stripped -- the ordinary
 *      argument-dispatched `(eq? x y)` with `x : A`;
 *   3. the call's own result type -- a return-dispatch method whose class var
 *      appears only in the result (`(:: (deserialize b) A)`).
 * Writes the identified TY_TYVAR into *out and returns true, or returns false
 * when no dispatch tyvar is present (a concrete receiver/result already baked the
 * right instance at elaboration).  Both the emit-time chokepoint
 * (emit_reresolve_disp_type) and the scan-time predicate
 * (emit_call_dispatches_on_spec_tyvar) route through here so they never disagree
 * about which position carries the dispatch variable. */
bool emit_dispatch_tyvar(const Expr *call, Type *out) {
    if (!call || call->kind != EX_CALL) return false;
    if (call->as.call_.n_args >= 1 && call->as.call_.args) {
        const Expr *recv = call->as.call_.args[0];
        for (const Expr *a = recv; a && a->kind == EX_ASCRIBE;
             a = a->as.ascribe_.inner) {
            if (a->type.kind == TY_TYVAR) { *out = a->type; return true; }
        }
        while (recv && recv->kind == EX_ASCRIBE) recv = recv->as.ascribe_.inner;
        if (recv && recv->type.kind == TY_TYVAR) { *out = recv->type; return true; }
    }
    if (call->type.kind == TY_TYVAR) { *out = call->type; return true; }
    /* constrained-hkt-spec-keeps-representative-instance: a higher-kinded class
     * method called on the abstract constructor has receiver `(m int)` and
     * result `(m b)` -- TY_APP spines whose HEAD is the dispatch variable.  The
     * bare-TY_TYVAR checks above are a kind-`*` assumption and never see it, so
     * the spec kept the env-ordered representative.  Hand back the whole spine;
     * emit_resolve_type grounds it per spec and selection matches head-wise.
     * Checked last so every kind-`*` case keeps its existing answer, and
     * deliberately inert for the scan-time predicate (which re-checks TY_TYVAR). */
    {
        const Expr *recv = (call->as.call_.n_args >= 1 && call->as.call_.args)
            ? call->as.call_.args[0] : NULL;
        while (recv && recv->kind == EX_ASCRIBE) recv = recv->as.ascribe_.inner;
        const Type *cands[2];
        uint8_t nc = 0;
        if (recv) cands[nc++] = &recv->type;
        cands[nc++] = &call->type;
        for (uint8_t i = 0; i < nc; i++) {
            const Type *t = cands[i];
            if (t->kind != TY_APP) continue;
            const Type *head = t;
            while (head->kind == TY_APP && head->as.app.fn) head = head->as.app.fn;
            if (head->kind == TY_TYVAR) { *out = *t; return true; }
        }
    }
    return false;
}

uint8_t emit_abi_constraint_var_bindings(const TypeClassInstance *inst,
                                         const Type *elems, uint8_t n_elems,
                                         AbiTypeBinding *out, uint8_t cap) {
    if (!inst || !elems || !out) return 0;
    uint8_t n = 0;
    for (uint8_t ci = 0; ci < inst->n_type_param_constraints && n < cap; ci++) {
        const TypeConstraint *tc = &inst->type_param_constraints[ci];
        if (!tc->tyvar || !tc->tyvar->name) continue;
        if (tc->param_idx < 0 || (uint8_t)tc->param_idx >= n_elems) continue;
        out[n].name = tc->tyvar->name;
        out[n].type = elems[tc->param_idx];
        n++;
    }
    return n;
}

bool emit_reresolve_disp_type(EmitCtx *ctx, const Expr *call,
                              Type *out_resolved, const Expr **out_dict) {
    if (!ctx || !ctx->current_abi_specialization || !call ||
        call->kind != EX_CALL) {
        return false;
    }
    const Expr *dict = call->as.call_.dict_arg;
    if (!dict || dict->kind != EX_DICT || !dict->as.dict_.instance) return false;
    if (dict->as.dict_.method_name[0] == '\0') return false;

    /* The dispatch type variable is the receiver, the ascribed receiver, or the
     * call's result type (see emit_dispatch_tyvar).  A concrete receiver/result
     * already baked the correct instance at elaboration, so we only act on a
     * genuine tyvar.  (return-dispatch-tyvar-silent-misdispatch.md) */
    Type disp_ty;
    bool have_disp = emit_dispatch_tyvar(call, &disp_ty);
    /* `disp_ty` is an owned (malloc'd) TY_APP spine only when it came from the
     * substitute-struct-app branch below; freed before every return so the
     * recovered concrete element type does not leak (the compiler/codegen path is
     * leak-checked).  Exposed by the nested `(Cons int)` element case (G2). */
    bool disp_ty_owned = false;
    /* nested-construct-byvalue (Gap #4): an ascribed return-dispatch method
     * whose `call->type` is a TY_APP *embedding* the class var -- e.g.
     * `(:: (dec tag) (Result A cstr))` for `(defclass Dec [a] (dec ... : (Result
     * a cstr)))` -- never matched the bare-TY_TYVAR check above, so the call kept
     * the int64-carrier representative instance (`__inst_Dec_dec_int`).  Recover
     * the dispatch type by matching the class method's declared result pattern
     * against the ascribed app and reading the subtype at the class-var position;
     * emit_resolve_type then grounds it through the active spec. */
    if (!have_disp && call->type.kind == TY_APP) {
        const TypeClass *tc_d = dict->as.dict_.instance->typeclass;
        if (tc_d && tc_d->n_type_params >= 1 && tc_d->type_params &&
            tc_d->type_params[0]) {
            const char *classvar = tc_d->type_params[0]->name;
            for (uint8_t mi = 0; mi < tc_d->n_methods; mi++) {
                const TypeClassMethod *m = &tc_d->methods[mi];
                if (!m->name || strcmp(m->name->name,
                                       dict->as.dict_.method_name) != 0)
                    continue;
                Type extracted;
                if (emit_pattern_extract_classvar(&m->return_type, &call->type,
                                                  classvar, &extracted)) {
                    disp_ty = extracted;
                    have_disp = true;
                }
                break;
            }
        }
    }
    /* nested-class-method-call-picks-the-first-instance: the receiver may
     * itself be a class-method call that this same pass re-resolves.
     *
     * Its ELABORATED type is the representative instance's -- the int64
     * carrier -- because the generic body was typed against whatever
     * carrier-compatible instance the elaborator picked first.  So
     * emit_dispatch_tyvar sees a concrete `int` receiver and bails, and the
     * OUTER call keeps `__inst_C_m_int` while the INNER one correctly
     * retargets.  In a float specialization that is
     *
     *     double  __ps0 = __inst_JS_join_float(x, y);   // inner: right
     *     int64_t __ps1 = __inst_JS_join_int(__ps0, y); // outer: WRONG
     *
     * -- a silent wrong answer (7 for 7.1), with `tur check` clean and cc
     * happy, since the double converts.
     *
     * When the inner method's declared return is the class variable itself
     * (`: a` -- every `a -> a -> a` shape: combine / join / meet, and the
     * reason a Semigroup-style family is entirely exposed to this), the
     * receiver's effective type IS the inner call's resolved dispatch type.
     * Recover it by re-resolving the receiver.  The recursion walks strictly
     * inward along receivers, so it terminates on any finite expression, and a
     * receiver whose own dispatch cannot be resolved leaves `have_disp` false
     * exactly as before. */
    if (!have_disp && call->as.call_.n_args >= 1 && call->as.call_.args) {
        const Expr *recv = call->as.call_.args[0];
        while (recv && recv->kind == EX_ASCRIBE) recv = recv->as.ascribe_.inner;
        if (recv && recv->kind == EX_CALL && recv->as.call_.dict_arg) {
            const Expr *idict = recv->as.call_.dict_arg;
            if (idict->kind == EX_DICT && idict->as.dict_.instance &&
                idict->as.dict_.method_name[0] != '\0') {
                const TypeClass *itc = idict->as.dict_.instance->typeclass;
                if (itc && itc->n_type_params >= 1 && itc->type_params &&
                    itc->type_params[0] && itc->type_params[0]->name) {
                    const char *icv = itc->type_params[0]->name;
                    for (uint8_t mi = 0; mi < itc->n_methods; mi++) {
                        const TypeClassMethod *m = &itc->methods[mi];
                        if (!m->name ||
                            strcmp(m->name->name, idict->as.dict_.method_name) != 0)
                            continue;
                        /* Only the bare-class-var return.  A method returning a
                         * compound shape needs the pattern extraction the TY_APP
                         * branch above does, not this. */
                        if (m->return_type.kind == TY_TYVAR &&
                            m->return_type.as.tyvar_.name &&
                            strcmp(m->return_type.as.tyvar_.name, icv) == 0) {
                            Type inner_resolved;
                            const Expr *inner_dict = NULL;
                            if (emit_reresolve_disp_type(ctx, recv, &inner_resolved,
                                                         &inner_dict)) {
                                disp_ty = inner_resolved;
                                have_disp = true;
                            }
                        }
                        break;
                    }
                }
            }
        }
    }

    /* constrained-instance-element-dispatch: the receiver may be a field
     * extraction from a parametric container whose element type was erased to
     * the int64 carrier at elaboration -- e.g. `(enc (.value x))` in the body of
     * `(definstance Enc [Option] [(Enc A)] ...)`, where `(.value x)` has the
     * struct's type-param `A` as its declared field type but elaborates with the
     * carrier `int` summary kind.  The genuine dispatch type is that type-param,
     * which the current spec resolves to a concrete element type (cstr/float/a
     * struct/...).  Recover it by substituting the spec-resolved receiver's
     * type-args into the field's declared type, so the inner class-method call
     * re-dispatches to the right instance per specialization instead of baking in
     * the carrier `int` representative.  Without this a constrained parametric
     * instance body silently misdispatches every non-int element type. */
    if (!have_disp && call->as.call_.n_args >= 1 && call->as.call_.args) {
        const Expr *recv = call->as.call_.args[0];
        while (recv && recv->kind == EX_ASCRIBE) recv = recv->as.ascribe_.inner;
        if (recv && recv->kind == EX_GET_FIELD && recv->as.get_field_.struct_expr) {
            const Expr *se = recv->as.get_field_.struct_expr;
            /* Resolve the receiver-container's concrete type for THIS spec.  When
             * the container is a spec parameter (the usual case -- the instance
             * method's `self`), emit_resolve_type leaves a parametric param type
             * such as `(Option A)` unsubstituted, so consult the spec's
             * arg_types[] by matching the binding against fn->params[] (the same
             * recovery emit_var_spec_arg_type / Path A.2 perform).  Otherwise fall
             * back to emit_resolve_type. */
            Type rt;
            bool have_rt = false;
            if (se->kind == EX_VAR)
                have_rt = emit_spec_arg_type_for_binding(ctx, se->as.var.binding, &rt);
            if (!have_rt) rt = emit_resolve_type(ctx, se->type);
            uint32_t fidx = recv->as.get_field_.field_idx;
            /* constrained-instance-element-dispatch (lowered record ADT
             * receiver): under defstruct-as-defadt the parametric container
             * is an ADT -- `(Option cstr)` is a TY_APP over the lowered record
             * ADT def.  Extract the AdtDef + element args, then substitute them
             * into the ctor field's declared full_type (the tyvar `A`) so the
             * inner `(enc (.value x))` re-dispatches on the concrete element.
             * (structdef-retirement DS-D: the former struct-app receiver path is
             * gone -- no struct-headed app forms.) */
            {
                AdtDef *ad = NULL;
                Type aargs[16];
                uint8_t an = 0;
                const CtorField *cf =
                    (recv->as.get_field_.adt_ctor &&
                     fidx < recv->as.get_field_.adt_ctor->n_fields)
                        ? &recv->as.get_field_.adt_ctor->fields[fidx] : NULL;
                if (cf && cf->full_type && cf->full_type->kind == TY_TYVAR &&
                    type_extract_adt_app(&rt, &ad, aargs, &an) && ad) {
                    Type fr = substitute_adt_app_type_owned(cf->full_type, ad, aargs);
                    if (fr.kind != TY_TYVAR && fr.kind != TY_UNKNOWN) {
                        disp_ty = fr;
                        have_disp = true;
                        disp_ty_owned = (fr.kind == TY_APP);
                    } else {
                        free_struct_app_type(fr);
                    }
                }
            }
        }
    }
    /* unascribed-carrier-helper-read-collapses-element-tyvar: the receiver may be
     * an UNASCRIBED generic carrier-helper read -- `(tag (vec-get v i))` -- whose
     * declared `:A` return collapses to the int64 carrier at elaboration, so
     * neither the bare-tyvar receiver check nor the ascription-to-tyvar branch
     * above fires (the call's elaborated type is `TY_INT`).  Recover the element
     * tyvar from the callee's own signature: a carrier helper's `result_full_type`
     * is its type-param `R`, and `R` also appears inside one of its parameters'
     * declared full types (`v : (Vec R)`).  Find that parameter, structurally
     * match its declared full type against the ACTUAL argument's type, and read
     * the subtype at `R`'s position -- e.g. matching `(Vec R)` against the actual
     * `(Vec A)` yields the constraint var `A`.  The downstream
     * `emit_resolve_type` + constraint-var `param_idx` grounding then resolves it
     * per specialization, matching the documented `(:: e A)` ascription idiom
     * without requiring the explicit ascription. */
    if (!have_disp && call->as.call_.n_args >= 1 && call->as.call_.args) {
        const Expr *recv = call->as.call_.args[0];
        while (recv && recv->kind == EX_ASCRIBE) recv = recv->as.ascribe_.inner;
        if (recv && recv->kind == EX_CALL && recv->as.call_.fn_binding &&
            recv->as.call_.args) {
            const Type *ft = &recv->as.call_.fn_binding->type;
            if (ft->kind == TY_FN && ft->as.fn.arg_full_types &&
                ft->as.fn.result_full_type &&
                ft->as.fn.result_full_type->kind == TY_TYVAR &&
                ft->as.fn.result_full_type->as.tyvar_.name) {
                const char *rname = ft->as.fn.result_full_type->as.tyvar_.name;
                uint32_t np = ft->as.fn.arity;
                for (uint8_t pi = 0;
                     pi < np && pi < recv->as.call_.n_args; pi++) {
                    const Type *pft = ft->as.fn.arg_full_types[pi];
                    const Expr *ae = recv->as.call_.args[pi];
                    if (!pft || !ae) continue;
                    Type extracted;
                    if (emit_pattern_extract_classvar(pft, &ae->type, rname,
                                                      &extracted)) {
                        disp_ty = extracted;
                        have_disp = true;
                        break;
                    }
                    /* SR2b: inside a constrained-instance body the container
                     * argument's elaborated type is often the BARE head ADT
                     * (`Option`, TY_ADT) rather than the applied `(Option A)`,
                     * so the structural match above has nothing to walk.  When
                     * the declared param's app head is that same ADT, dispatch
                     * on the accessor's declared result tyvar itself -- the
                     * constraint-var grounding below (param_idx into the
                     * class-var binding's type args) resolves it per spec,
                     * exactly as it does for the field-read receiver shape.
                     * (constrained-instance-element-dispatch: `(enc (unwrap
                     * x))` kept baking `__inst_Enc_enc_int` for float / cstr /
                     * Box elements.) */
                    if (pft->kind == TY_APP && ae->type.kind == TY_ADT) {
                        const Type *ph = pft;
                        while (ph->kind == TY_APP && ph->as.app.fn)
                            ph = ph->as.app.fn;
                        if (ph->kind == TY_ADT &&
                            ph->as.adt_.def == ae->type.as.adt_.def) {
                            disp_ty = *ft->as.fn.result_full_type;
                            have_disp = true;
                            break;
                        }
                    }
                }
            }
        }
    }
    if (!have_disp) return false;

    Type resolved = emit_resolve_type(ctx, disp_ty);
    /* emit_resolve_type produced an arena-backed (or unchanged-scalar) result; the
     * owned malloc'd `disp_ty` spine is no longer needed. */
    if (disp_ty_owned) { free_struct_app_type(disp_ty); disp_ty_owned = false; }
    /* nested-construct-byvalue (Gap #4): a constrained parametric instance binds
     * its constraint var (`(Dec A)` -> tyvar `A`) only implicitly -- the spec's
     * stored bindings carry just the class var (`a -> (Option cstr)`), not `A`.
     * When `disp_ty` is that constraint var and the direct spec lookup leaves it
     * unbound, resolve it the way emit_abi_register_call's augmentation does:
     * the constraint records a `param_idx` into the receiver's (class-var's)
     * type-arg list, so extract that element from the class-var binding. */
    if (resolved.kind == TY_TYVAR && resolved.as.tyvar_.name &&
        ctx->current_abi_specialization &&
        ctx->current_abi_specialization->fn &&
        ctx->current_abi_specialization->fn->owner_instance &&
        ctx->current_abi_specialization->n_bindings >= 1) {
        const TypeClassInstance *inst =
            ctx->current_abi_specialization->fn->owner_instance;
        Type recv = ctx->current_abi_specialization->bindings[0].type;
        AdtDef *rad = NULL; Type rargs[16]; uint8_t rn = 0;
        /* CONV-S2: the class-var receiver is a lowered record ADT-app
         * (`(Vec bool)`/`(Cons (Option int))`) under defstruct-as-defadt -- so a
         * constrained instance over a lowered container (`(definstance Tag [Vec]
         * [(Tag A)] ...)`) would otherwise leave its element constraint var
         * unbound and the inner `(tag (:: (vec-get v i) A))` keep the baked
         * carrier representative (wrong instance / a segfault when the element's
         * real ABI differs).  (structdef-retirement DS-D: the former struct-app
         * extraction is gone -- no struct-headed app forms.) */
        if (type_extract_adt_app(&recv, &rad, rargs, &rn)) {
            /* Route the param_idx->element mapping through the shared chokepoint
             * kernel (extraction stays here), then pick the entry naming this
             * still-unbound constraint var. */
            AbiTypeBinding cb[ABI_TYPE_BINDINGS_MAX];
            uint8_t ncb = emit_abi_constraint_var_bindings(inst, rargs, rn, cb,
                                                           ABI_TYPE_BINDINGS_MAX);
            for (uint8_t ci = 0; ci < ncb; ci++) {
                if (cb[ci].name &&
                    strcmp(cb[ci].name, resolved.as.tyvar_.name) == 0) {
                    resolved = cb[ci].type;
                    break;
                }
            }
        }
    }
    if (resolved.kind == TY_TYVAR) return false; /* still unbound: keep base/repr */
    /* Edge 1 (generic-show-wrapper-cps-monomorphization-plan): a dispatch type with
     * a CONCRETE HEAD but an unresolved element -- e.g. `(show x)` inside a
     * `show-line` clone specialized on `(Vec ?)` from `(show-line (vec-new))`,
     * where the empty container never let the element type be inferred -- is a
     * TY_APP whose spine carries a tyvar.  Instance SELECTION keys on the head
     * (Show[Vec], matched head-wise by emit_inst_head_matches), and the element is
     * used only inside the instance body for per-element dispatch, which an empty
     * container never runs.  So let the caller's authoritative head-match resolve
     * it rather than tripping the R3 assertion (Debug) / silently selecting a
     * carrier `__inst_*` (Release).  This is safe: a TY_APP dispatch type has no
     * carrier-rep suffix fallback (emit_inst_suffix_component(TY_APP) == NULL), so
     * emit_reresolve_method_call either matches by head or yields NULL (keep
     * baked).  Matches the direct `(show (vec-new))` path, which renders `[]`.
     * If the HEAD itself is unresolved there is nothing to select -- keep
     * base/repr, as for a bare tyvar. */
    if (type_spine_has_tyvar(&resolved, 0)) {
        const Type *head = &resolved;
        while (head->kind == TY_APP && head->as.app.fn) head = head->as.app.fn;
        if (head->kind == TY_TYVAR || head->kind == TY_UNKNOWN) return false;
        *out_resolved = resolved;
        *out_dict = dict;
        return true;
    }
    /* R3 gate: a successful re-resolution must yield a concrete dispatch type.
     * A TY_APP whose spine still carries a tyvar would silently select the
     * carrier-representative `__inst_*` -- the routing hole this asserts away. */
    emit_abi_assert_routed_concrete(ctx, &resolved, "emit_reresolve_disp_type", true);
    *out_resolved = resolved;
    *out_dict = dict;
    return true;
}

/* nested-construct-byvalue: scan-time companion to emit_reresolve_method_call --
 * returns the FnDef of the concrete instance method a constrained-instance body
 * re-dispatches to (e.g. `__inst_Dec_dec_cstr`), so the ABI scan can mark that
 * instance live.  Without this the re-dispatched callee is referenced by an
 * emitted spec body but its instance is pruned as dead (undefined reference). */
FnDef *emit_reresolve_method_fndef(EmitCtx *ctx, const Expr *call) {
    Type resolved;
    const Expr *dict = NULL;
    if (!emit_reresolve_disp_type(ctx, call, &resolved, &dict)) return NULL;
    const TypeClass *tc = dict->as.dict_.instance->typeclass;
    if (!tc) return NULL;
    return emit_concrete_inst_method_fndef(ctx, tc, resolved,
                                           dict->as.dict_.method_name);
}

char *emit_reresolve_method_call(EmitCtx *ctx, const Expr *call) {
    Type resolved;
    const Expr *dict = NULL;
    if (!emit_reresolve_disp_type(ctx, call, &resolved, &dict)) return NULL;

    /* Gap H: when the concrete instance can be located, use its method impl's
     * authoritative emitted symbol.  This is the only spelling that stays
     * correct for parametric / multi-parameter instance heads (e.g.
     * `StorageOps [(Dense Pos) Pos]`), whose suffix the single-component
     * reconstruction below cannot reproduce.  Fall through to reconstruction
     * only when no concrete instance matches. */
    {
        const TypeClass *tc_lookup = dict->as.dict_.instance->typeclass;
        if (tc_lookup) {
            char *authoritative = emit_concrete_inst_method_name(
                ctx, tc_lookup, resolved, dict->as.dict_.method_name);
            if (authoritative) return authoritative;
        }
    }

    const char *component = emit_inst_suffix_component(resolved.kind);
    if (!component) return NULL;

    const TypeClass *tc = dict->as.dict_.instance->typeclass;
    if (!tc || !tc->name) return NULL;

    Buf nm; buf_init(&nm);
    buf_printf(&nm, "__inst_%s_%s_%s", tc->name->name,
               dict->as.dict_.method_name, component);
    buf_putc(&nm, '\0');
    char *result = strdup(nm.data);
    buf_free(&nm);
    if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
    return result;
}

/* GHE struct-receiver ABI bridge: when emit_reresolve_method_call retargets a
 * constrained-generic method call (inside an ABI specialization) to a concrete
 * instance whose receiver is a pass-by-pointer struct/ADT, that instance method
 * is emitted as `__inst_<Class>_<method>_<T>(const T *self, ...)` -- but the
 * spec clone holds the receiver argument by value.  The receiver must therefore
 * be passed by address (`&tmp`) to match the `const T *` formal.
 *
 * Returns true only when the re-resolved callee genuinely takes the receiver by
 * pointer: the dispatch tyvar is the receiver argument (arg 0), it resolves to a
 * pass-by-ptr struct/ADT, and the selected instance method emits its receiver as
 * `const T *` (mirrors emit_fns.c: a non-inline-C, non-closure struct param
 * above the pass-by-ptr threshold).  Returns false for the int-carrier base
 * clone (no re-resolution), single-field/scalar receivers that ride the int64
 * carrier's by-value ABI, and inline-C/closure instance bodies (which declare
 * struct params by value).  Without this bridge a genuinely-sized struct
 * receiver passed a `T` to a `const T *` formal -- a hard cc type error that
 * single-field structs only "worked around" by register-class coincidence. */
/* constrained-generic-monomorph-passbyptr-arg: the same question for ANY
 * argument `i` of a dictionary-dispatched call re-resolved in this spec.  The
 * re-resolved instance method takes every parameter that crosses the
 * pass-by-ptr threshold as `const T *`, not only the receiver: `(join p q)`
 * inside `merge-two [T] [(Join T)]` at T := Reg spilled `p` and passed `q` by
 * value.  An argument typed by a type variable resolves through the spec; a
 * concrete one is its own type (the representative's binding said nothing
 * about it). */
bool emit_reresolved_param_is_by_ptr(EmitCtx *ctx, const Expr *call, uint32_t i) {
    if (!ctx || !ctx->current_abi_specialization || !call ||
        call->kind != EX_CALL) {
        return false;
    }
    const Expr *dict = call->as.call_.dict_arg;
    if (!dict || dict->kind != EX_DICT || !dict->as.dict_.instance) return false;
    if (dict->as.dict_.method_name[0] == '\0') return false;
    if (i >= call->as.call_.n_args || !call->as.call_.args) return false;
    const Expr *arg = call->as.call_.args[i];
    while (arg && arg->kind == EX_ASCRIBE) arg = arg->as.ascribe_.inner;
    if (!arg) return false;
    /* The receiver (arg 0) keeps its original contract: only a type-variable
     * receiver needs the bridge (a return-dispatch method has none). */
    if (i == 0 && arg->type.kind != TY_TYVAR) return false;

    Type resolved = emit_resolve_type(ctx, arg->type);
    if (resolved.kind != TY_STRUCT && resolved.kind != TY_ADT &&
        resolved.kind != TY_APP) {
        return false; /* scalar carrier / still unbound -> int base clone */
    }
    if (!type_struct_pass_by_ptr(resolved)) return false;

    /* Confirm the selected instance method emits the parameter by pointer.
     * Match the dict's (mangled) method name against the instance's typeclass
     * methods to locate the impl FnDef; an inline-C / closure body declares
     * its params by value even above the pass-by-ptr threshold (emit_fns.c
     * §737). */
    TypeClassInstance *inst = dict->as.dict_.instance;
    const TypeClass *tc = inst->typeclass;
    if (!tc) return false;
    for (uint8_t k = 0; k < tc->n_methods && k < inst->n_method_impls; k++) {
        if (!tc->methods[k].name) continue;
        char mangled[128];
        tur_mangle_ident(tc->methods[k].name->name, mangled, sizeof(mangled));
        if (strcmp(mangled, dict->as.dict_.method_name) != 0) continue;
        FnDef *impl = inst->method_impls[k];
        if (!impl) return false;
        if (impl->closure) return false;
        /* Inline-C instance bodies declare their struct params BY VALUE even
         * above the pass-by-ptr threshold (emit_fns.c §643/737 keys this off
         * `fd->body->kind == EX_INLINE_C`, not the binding flag).  Mirror that
         * exact predicate here: a `body_is_inline_c` cached on the binding is
         * not reliably set for instance-method FnDefs, so checking it alone let
         * the address-of bridge fire on an inline-C receiver -- passing `&tmp`
         * (a `T *`) to a by-value `T self` formal.  That re-triggered the very
         * ABI mismatch #439 set out to fix, but in the opposite direction, the
         * moment a generic helper forced the instance method to be emitted. */
        if (impl->body && impl->body->kind == EX_INLINE_C) return false;
        if (impl->binding && impl->binding->body_is_inline_c) return false;
        return true;
    }
    return false;
}

bool emit_reresolved_receiver_is_by_ptr(EmitCtx *ctx, const Expr *call) {
    return emit_reresolved_param_is_by_ptr(ctx, call, 0);
}

static char *capture_env_access(EmitCtx *ctx, const Binding *b);

/* forall-dict-pass-multi-constraint-hkt-plan (Task 1.4): the index of the dict
 * slot whose class OWNS this method call, or -1 when the call is not a dict-param
 * dispatch.  Keys on the method's instance class, so `fmap`/`show` land on their
 * respective slots regardless of constraint order. */
int emit_call_dict_param_dispatch_index(EmitCtx *ctx, const Expr *call) {
    if (!(ctx && ctx->dict_dispatch_n > 0 && call && call->kind == EX_CALL &&
          call->as.call_.dict_arg && call->as.call_.dict_arg->kind == EX_DICT &&
          call->as.call_.dict_arg->as.dict_.instance &&
          call->as.call_.dict_arg->as.dict_.instance->typeclass &&
          call->as.call_.dict_arg->as.dict_.method_name[0] != '\0'))
        return -1;
    const TypeClass *mtc = call->as.call_.dict_arg->as.dict_.instance->typeclass;
    for (uint8_t k = 0; k < ctx->dict_dispatch_n; k++)
        if (ctx->dict_dispatch_classes[k] == mtc) return (int)k;
    return -1;
}

/* forall-dict-pass-nested-lambda-dispatch-plan (Phase 3) +
 * forall-dict-pass-nested-mapper-general-plan (Phase 1): the index of the
 * captured-dict slot whose class OWNS this method call, or -1 when the call is
 * not an env-dict dispatch.  Keys on the method's instance class, so each
 * dispatched class lands on its own captured dict regardless of capture order. */
int emit_call_dict_env_dispatch_index(EmitCtx *ctx, const Expr *call) {
    if (!(ctx && ctx->cur_dict_env_n > 0 && call && call->kind == EX_CALL &&
          call->as.call_.dict_arg && call->as.call_.dict_arg->kind == EX_DICT &&
          call->as.call_.dict_arg->as.dict_.instance &&
          call->as.call_.dict_arg->as.dict_.instance->typeclass &&
          call->as.call_.dict_arg->as.dict_.method_name[0] != '\0'))
        return -1;
    /* The dispatch must be on the constraint's own type variable -- a concrete
     * same-class call in the same mapper body (e.g. `(show 42)` alongside
     * `(show x)`) is instance-resolved and must NOT be routed through the
     * polymorphic env dict.  Mirrors the elab-side gate
     * (call_dispatched_constraint_class): receiver-directed keys on a bare
     * tyvar receiver; return-directed (`pure`/`empty` -- Route B,
     * constrained-hkt-lifted-lambda-keeps-representative-instance) keys on the
     * result being tyvar-headed. */
    {
        /* The receiver test keys on the HEAD of the receiver's type, so a
         * higher-kinded `(m a)` receiver qualifies (hkt-generic-nested-bind-
         * result-type); it must match call_dispatched_constraint_class. */
        bool recv_is_tyvar = false;
        if (call->as.call_.n_args >= 1 && call->as.call_.args &&
            call->as.call_.args[0]) {
            const Type *rh = &call->as.call_.args[0]->type;
            while (rh->kind == TY_APP && rh->as.app.fn) rh = rh->as.app.fn;
            recv_is_tyvar = (rh->kind == TY_TYVAR);
        }
        const Type *h = &call->type;
        while (h->kind == TY_APP && h->as.app.fn) h = h->as.app.fn;
        if (!recv_is_tyvar && h->kind != TY_TYVAR) return -1;
    }
    const TypeClass *mtc = call->as.call_.dict_arg->as.dict_.instance->typeclass;
    for (uint8_t k = 0; k < ctx->cur_dict_env_n; k++)
        if (ctx->cur_dict_env_classes[k] == mtc) return (int)k;
    return -1;
}

bool emit_call_is_dict_env_dispatch(EmitCtx *ctx, const Expr *call) {
    return emit_call_dict_env_dispatch_index(ctx, call) >= 0;
}

bool emit_call_is_dict_param_dispatch(EmitCtx *ctx, const Expr *call) {
    return emit_call_dict_param_dispatch_index(ctx, call) >= 0 ||
           emit_call_is_dict_env_dispatch(ctx, call);
}

/* The plain (un-specialised) spelling of a callee: a global's raw C symbol,
 * or -- let-alias-of-fn-param-call-undeclared -- a LOCAL's declared name.  The
 * three early exits of emit_call_name below used raw_name_for_binding for
 * both, so a zero-argument call through a let-bound poly-fn alias `h` was
 * emitted `h.fn(h.env)` against a declaration `tur_poly_fn_t h_1608` (the
 * same declared-vs-used mismatch the function's closing comment describes
 * for the general path, one exit earlier). */
static char *call_name_plain(EmitCtx *ctx, const Binding *b) {
    if (b && !b->is_global) return name_for_binding(ctx, b);
    return raw_name_for_binding(b);
}

/* A row-kinded variable (`^&` rows) never changes the C ABI, exactly as
 * emit_abi_type_has_concrete_named_tyvar says; every other tyvar does. */
bool emit_abi_type_is_open(const Type *t) {
    if (!t) return false;
    if (t->kind == TY_TYVAR) return t->hkt_kind != KIND_TYPEROW;
    if (t->kind == TY_APP)
        return emit_abi_type_is_open(t->as.app.fn) ||
               emit_abi_type_is_open(t->as.app.arg);
    return false;
}

bool emit_expr_abstract_under_active_spec(EmitCtx *ctx, const Expr *e) {
    if (!ctx || !ctx->current_abi_specialization || !e) return false;
    Type rt = emit_resolve_type(ctx, e->type);
    return emit_abi_type_is_open(&rt);
}

bool emit_call_abstract_under_active_spec(EmitCtx *ctx, const Expr *call) {
    if (!ctx || !ctx->current_abi_specialization || !call ||
        call->kind != EX_CALL) return false;
    for (uint32_t ai = 0; ai < call->as.call_.n_args; ai++)
        if (emit_expr_abstract_under_active_spec(ctx, call->as.call_.args[ai]))
            return true;
    return false;
}

/* class-var-applied-result: does this call dispatch through a runtime dict
 * slot that hands back the WORD (dict_slot_result_is_word_scalar)?  Then its
 * C value is an int64 -- for an applied class result, the box the carrier
 * spells `(Option A)` with -- and no consumer may treat it as a by-value
 * aggregate to spill.  The dict source and slot are resolved exactly as
 * emit_call_name resolves them below. */
bool emit_call_dispatches_word_result(EmitCtx *ctx, const Expr *call) {
    if (!ctx || !call || call->kind != EX_CALL || !call->as.call_.dict_arg ||
        call->as.call_.dict_arg->kind != EX_DICT)
        return false;
    int ddk = emit_call_dict_param_dispatch_index(ctx, call);
    int dek = ddk >= 0 ? -1 : emit_call_dict_env_dispatch_index(ctx, call);
    const TypeClass *tc = ddk >= 0 ? ctx->dict_dispatch_classes[ddk]
                        : dek >= 0 ? ctx->cur_dict_env_classes[dek] : NULL;
    if (!tc) return false;
    const char *mname = call->as.call_.dict_arg->as.dict_.method_name;
    for (uint8_t i = 0; i < tc->n_methods; i++) {
        char mm[64];
        tur_mangle_ident(tc->methods[i].name->name, mm, sizeof(mm));
        if (strcmp(mm, mname) != 0) continue;
        const TypeClassInstance *repr = call->as.call_.dict_arg->as.dict_.instance;
        const FnDef *mimpl = (repr && i < repr->n_method_impls)
            ? repr->method_impls[i] : NULL;
        return dict_slot_result_is_word_scalar(tc, (int)i, mimpl, NULL);
    }
    return false;
}

/* constrained-generic-relay-borrows-sibling-clone: a call recorded only under
 * a SIBLING outer spec -- `(ei v)` inside `eu`, minted `ei__spec__W` under
 * `eu__spec__W` and nothing under `eu__spec__int` (no ABI change at int) --
 * was routed to the sibling's clone by the fallback below, so `(eu 40)` called
 * the W instance's spec on an int: a segfault, or with a payload that does not
 * fault, the wrong instance's answer.  Borrow a sibling's clone only when its
 * parameter C types are what this call's arguments resolve to here. */
static bool cross_spec_clone_fits_call(EmitCtx *ctx, const Expr *call,
                                       const char *clone_name) {
    if (!ctx || !call || call->kind != EX_CALL || !clone_name) return true;
    const EmitAbiSpecialization *sp = NULL;
    for (uint32_t i = 0; i < ctx->n_abi_specializations && !sp; i++)
        if (ctx->abi_specializations[i].clone_name &&
            strcmp(ctx->abi_specializations[i].clone_name, clone_name) == 0)
            sp = &ctx->abi_specializations[i];
    if (!sp || sp->n_args != call->as.call_.n_args) return true;
    for (uint32_t ai = 0; ai < sp->n_args; ai++) {
        const Expr *a = call->as.call_.args[ai];
        while (a && a->kind == EX_ASCRIBE) a = a->as.ascribe_.inner;
        if (!a) continue;
        Type at = emit_resolve_type(ctx, a->type);
        if (emit_abi_type_is_open(&at) || at.kind == TY_UNKNOWN) continue;
        const char *want = emit_type_c_name(ctx, emit_resolve_type(ctx, sp->arg_types[ai]));
        const char *have = emit_type_c_name(ctx, at);
        if (want && have && strcmp(want, have) != 0) return false;
    }
    return true;
}

char *emit_call_name(EmitCtx *ctx, const Expr *call, const Binding *b) {
    const Expr *cur = NULL;
    /* MB1 (constrained-hkt-forall-mode-b-plan): while emitting a dict-clone
     * body, a class-method call on the constrained type variable (one carrying a
     * `dict_arg` for the clone's class) dispatches through the runtime dict param
     * -- `((<ret> (*)(int64_t...))((void **)(intptr_t)<dict>)[<slot>])` -- exactly
     * like the existential witness path, but sourcing the witness from the dict
     * parameter instead of an `open`-bound table.  The method slot is its index
     * in the class dict layout (dict struct fields are emitted in class-method
     * order, emit_stmt.c). */
    /* Resolve the dict SOURCE for a class-method dispatch on the constrained
     * var: either a dict-clone param (`ddk >= 0`) or -- for a nested mapper
     * closure converted by forall-dict-pass-nested-lambda-dispatch-plan Phase 2
     * -- the CAPTURED dict read from the closure env (`env->dict`).  The rest of
     * the emission (return type, param signature, slot) is identical. */
    int ddk = emit_call_dict_param_dispatch_index(ctx, call);
    int dek = ddk >= 0 ? -1 : emit_call_dict_env_dispatch_index(ctx, call);
    const TypeClass *disp_tc = NULL;
    char *disp_dict_src = NULL;  /* owned */
    if (ddk >= 0) {
        disp_tc = ctx->dict_dispatch_classes[ddk];
        disp_dict_src = strdup(ctx->dict_dispatch_param_cnames[ddk]);
    } else if (dek >= 0) {
        disp_tc = ctx->cur_dict_env_classes[dek];
        disp_dict_src = capture_env_access(ctx, ctx->cur_dict_env_bindings[dek]);
        if (!disp_dict_src)
            disp_dict_src = raw_name_for_binding(ctx->cur_dict_env_bindings[dek]);
    }
    if (disp_tc) {
        const TypeClass *tc = disp_tc;
        const char *mname = call->as.call_.dict_arg->as.dict_.method_name;
        int slot = -1;
        for (uint8_t i = 0; i < tc->n_methods; i++) {
            char mm[64];
            tur_mangle_ident(tc->methods[i].name->name, mm, sizeof(mm));
            if (strcmp(mm, mname) == 0) { slot = (int)i; break; }
        }
        if (slot >= 0) {
            uint32_t na = call->as.call_.n_args;
            const TypeClassInstance *repr =
                call->as.call_.dict_arg->as.dict_.instance;
            const FnDef *mimpl = (repr && slot < repr->n_method_impls)
                ? repr->method_impls[slot] : NULL;
            Buf b2; buf_init(&b2);
            /* One diagnostic per dispatch site, not one per offending param. */
            /* MB2.5 (constrained-hkt-forall-mode-b-plan): the dispatched RETURN
             * type must mirror the dict field (emit_stmt.c:581-603), which stores
             * the carrier instance method -- for a class-var-typed result (`(f b)`)
             * that is the int64 carrier, NOT the call's by-value aggregate result
             * (`call->type` resolves to `Maybe int` for a by-value functor).  Using
             * `call->type` here made the fn-ptr cast return `tur_adt_Maybe__int`
             * while the dict slot returns int64 -- an incompatible-types cc error.
             * Derive it from the representative instance method's declared result,
             * exactly as the dict struct field does; carrier-compatible functors
             * (Box) already yield int64 this way, so this is inert for them. */
            const char *ret_c = NULL;
            if (mimpl && mimpl->binding &&
                mimpl->binding->type.kind == TY_FN) {
                Type *rft = mimpl->binding->type.as.fn.result_full_type;
                Type ret_type = rft ? *rft
                    : emit_type_from_kind(
                          mimpl->binding->type.as.fn.result_kind);
                if (ret_type.kind == TY_FN) {
                    const char *carrier =
                        emit_inst_fn_return_carrier(mimpl, &ret_type);
                    ret_c = carrier ? carrier : type_c_name(ret_type);
                } else {
                    ret_c = type_c_name(ret_type);
                }
            }
            if (!ret_c) ret_c = emit_type_c_name(ctx, call->type);
            /* dict-slot-classvar-scalar-result: the slot returns the word. */
            if (dict_slot_result_is_word_scalar(tc, slot, mimpl, NULL))
                ret_c = "int64_t";
            buf_printf(&b2, "((%s (*)(", ret_c);
            /* MB2 (constrained-hkt-forall-mode-b-plan): the dispatched signature
             * must mirror the dict field layout (emit_stmt.c) exactly -- a
             * poly-fn method param (e.g. `fmap`'s `g : (fn [a] b)`) is a
             * `tur_poly_fn_t`, not the int64 carrier; a class-var-typed param
             * (`(f a)`) is the carrier.  Derive the per-param C type from the
             * representative instance's method impl; fall back to all-int64 (the
             * MB1 case: every param is the class-var receiver). */
            if (mimpl && mimpl->n_params == na && mimpl->param_types) {
                if (na == 0) buf_puts(&b2, "void");
                for (uint32_t i = 0; i < na; i++) {
                    if (i) buf_puts(&b2, ", ");
                    if (mimpl->params && mimpl->params[i]->is_poly_fn) {
                        buf_puts(&b2, "tur_poly_fn_t");
                    } else {
                        Type pt = mimpl->param_types[i];
                        if (dict_slot_param_is_word_scalar(tc, slot, mimpl, i))
                            /* dict-slot-classvar-scalar-param: the slot holds
                             * a word-taking wrapper for every instance whose
                             * class-variable parameter is not the word. */
                            buf_puts(&b2, "int64_t");
                        else if (type_struct_pass_by_ptr(pt))
                            buf_printf(&b2, "const %s *", type_c_name(pt));
                        else if (emit_type_is_byvalue_adt(ctx, pt)) {
                            /* D8 piece 2 (forall-dict-byvalue-receiver): the
                             * carrier, matching the dict SLOT, which now holds
                             * a per-instance wrapper that derefs it
                             * (emit_stmt.c).  This used to be a hard error --
                             * the slot held the raw impl, whose parameter is
                             * the struct by value, so the pun handed it a
                             * pointer.  Both ends agree now, so the shape is
                             * supported rather than guarded.
                             *
                             * The REPRESENTATIVE-instance concern the guard
                             * also raised is answered by the same wrapper: every
                             * instance's slot is `(carrier) -> ret`, so there is
                             * one signature to represent rather than two
                             * disagreeing layouts. */
                            buf_puts(&b2, "int64_t");
                        }
                        else
                            buf_puts(&b2, type_c_name(pt));
                    }
                }
            } else {
                if (na == 0) buf_puts(&b2, "void");
                for (uint32_t i = 0; i < na; i++)
                    buf_printf(&b2, "%sint64_t", i ? ", " : "");
            }
            buf_printf(&b2, "))((void **)(intptr_t)%s)[%d])",
                       disp_dict_src, slot);
            buf_putc(&b2, '\0');
            char *out = strdup(b2.data);
            buf_free(&b2);
            free(disp_dict_src);
            return out;
        }
        free(disp_dict_src);
    }
    /* GHE: typeclass-method dispatch inside a monomorphized constrained generic
     * takes precedence over the generic-function specialization lookup below. */
    {
        /* G2 (carrier<->concrete nested dispatch): if the ABI scan minted a
         * per-instantiation by-value spec for this call under the active outer
         * (emit_abi_try_nested_instance_dispatch_redirect), prefer that recorded
         * spec over the carrier-base re-resolution -- the recorded clone takes the
         * concrete container by value (`Cons__int *`), the base takes int64. */
        const char *active_outer = ctx && ctx->current_abi_specialization
            ? ctx->current_abi_specialization->clone_name : NULL;
        bool has_recorded_spec = false;
        if (ctx) {
            for (uint32_t i = 0; i < ctx->n_specialized_calls; i++) {
                if (ctx->specialized_call_exprs[i] == call &&
                    ctx->specialized_call_outer[i] == active_outer) {
                    has_recorded_spec = true;
                    break;
                }
            }
        }
        if (!has_recorded_spec) {
            char *reresolved = emit_reresolve_method_call(ctx, call);
            if (reresolved) return reresolved;
        }
    }
    if (ctx && call) {
        /* M5 Finding 7: the same source-body call Expr* is recorded once per
         * outer spec (per element type for a shared instance-method body).
         * Prefer the entry whose recorded outer spec matches the CURRENT active
         * spec, so each element-type spec body emits its own callee clone;
         * fall back to the first entry (top-level / single-spec, unchanged). */
        const char *active_outer = ctx->current_abi_specialization
            ? ctx->current_abi_specialization->clone_name : NULL;
        /* option-consumer-retype-byvalue step 2: a `^construct` callee
         * (`some`/`none`/`ok`/`err`) emitted inside a spec whose own return is
         * the int64 carrier must produce the carrier box, not a by-value clone.
         * This case arises in a pure-Turmeric `option-map`/`result-map` body
         * when the `(fn [A] B)` closure leaves the result element `B`
         * unresolved (e.g. a `ptr<void>` capturing closure): elab mints a spec
         * whose declared return collapses to `int64_t`, but the body's
         * `(some ...)` Expr was *also* recorded under a sibling spec where `B`
         * resolved (returning `Option__int` by value).  The cross-spec / by-args
         * fallbacks below would route the construct to that by-value clone,
         * assigning an `Option__int` aggregate into the carrier `int64_t`
         * local -- a hard cc error.  Suppress those fallbacks here so the
         * construct stays on its carrier base; the EXACT per-Expr* match (an
         * entry recorded under THIS active outer, honoured below) is unaffected. */
        bool construct_into_carrier =
            b && b->is_construct_template && ctx->current_abi_specialization &&
            strcmp(emit_type_c_name(ctx, ctx->current_abi_specialization->result_type),
                   "int64_t") == 0;
        /* colored-generic-tyvar-elemented-param-sig-rejects: under an ERASED
         * outer clone this call's args are still the carrier; a sibling's
         * by-value clone is never the right callee (see the helper). */
        bool abstract_here = emit_call_abstract_under_active_spec(ctx, call);
        const char *matched = NULL;
        bool saw = false;
        bool saw_any = false;
        for (uint32_t i = 0; i < ctx->n_specialized_calls; i++) {
            if (ctx->specialized_call_exprs[i] != call) continue;
            saw_any = true;
            if (ctx->specialized_call_outer[i] == active_outer) {
                matched = ctx->specialized_call_names[i];
                saw = true;
                break;
            }
            /* Cross-spec fallback only inside a spec (active_outer != NULL); a
             * carrier base / top-level emit must require an exact NULL-outer
             * match so it never routes a call to a spec-scoped clone with a
             * different return ABI (M2-completion primitive-payload construct). */
            if (active_outer != NULL && !saw && !construct_into_carrier &&
                !abstract_here &&
                cross_spec_clone_fits_call(ctx, call,
                                           ctx->specialized_call_names[i])) {
                matched = ctx->specialized_call_names[i];
                saw = true;
            }
        }
        if (saw) {
            char *name = strdup(matched);
            if (!name) { fprintf(stderr, "tur: oom\n"); abort(); }
            return name;
        }
        /* Construct-into-carrier (see above): force the carrier base callee so a
         * by-value sibling clone never leaks into a carrier-returning spec body. */
        if (construct_into_carrier) {
            if (b) {
                char *captured = capture_env_access(ctx, b);
                if (captured) return captured;
            }
            return call_name_plain(ctx, b);
        }
        /* If this call was recorded under a spec outer but none matches the
         * active (NULL) outer, it is a spec-scoped specialization (e.g. a
         * by-value-return construct spec) -- the carrier base / top-level emit
         * must use the plain carrier callee, not the by-args lookup below which
         * cannot distinguish a return-only-differentiated spec from the carrier
         * (identical arg types).  Skip to the carrier name. */
        if (saw_any && active_outer == NULL) {
            if (b) {
                char *captured = capture_env_access(ctx, b);
                if (captured) return captured;
            }
            return call_name_plain(ctx, b);
        }
        /* option-consumer-retype-byvalue step 2: a 0-arg call (e.g. a
         * `(none)` / `(empty)` constructor) carries no argument types to
         * disambiguate one spec from another, so the by-args lookup below
         * degenerates to "first spec of this binding" and would route every
         * such call -- including carrier-context ones -- to a by-value spec
         * the moment one is interned.  The per-Expr* recording above is the
         * only sound disambiguator; an unrecorded 0-arg constructor stays on
         * the carrier callee.
         *
         * The SAME hazard applies to an N-arg `^construct` callee
         * (`(ok x)` / `(err e)` / `(some x)`): its by-value spec and the int64
         * carrier base share identical argument types and differ only in
         * return ABI, so the by-args match cannot tell them apart.  Once a
         * pure-Turmeric body (e.g. `result-map`) interns a by-value
         * `ok__spec__Result__int__int`, the by-args fallback would route an
         * unrelated carrier-context `(ok? (ok 1))` to it (assigning a
         * `Result__int__int` aggregate into a carrier `int64_t` slot -- a cc
         * error).  A construct call that genuinely needs the by-value spec was
         * recorded per-Expr* and is honoured by the exact-match path above; an
         * unrecorded construct call stays on the carrier base. */
        if (call->kind == EX_CALL &&
            (call->as.call_.n_args == 0 || (b && b->is_construct_template))) {
            if (b) {
                char *captured = capture_env_access(ctx, b);
                if (captured) return captured;
            }
            return call_name_plain(ctx, b);
        }
        if (call->kind == EX_CALL && b) {
            for (uint32_t si = 0; si < ctx->n_abi_specializations; si++) {
                const EmitAbiSpecialization *spec = &ctx->abi_specializations[si];
                if (spec->binding != b || spec->n_args != call->as.call_.n_args) continue;
                /* G6: skip a return-differentiated sibling spec (e.g. the bool
                 * `re-cata` clone for an int-result call) -- lockstep with
                 * find_matched_abi_spec's identical guard. */
                if (emit_spec_result_mismatch(ctx, emit_resolve_type(ctx, call->type),
                                              spec->result_type)) {
                    continue;
                }
                bool args_match = true;
                for (uint32_t ai = 0; ai < call->as.call_.n_args; ai++) {
                    cur = call->as.call_.args[ai];
                    while (cur && cur->kind == EX_ASCRIBE) cur = cur->as.ascribe_.inner;
                    Type actual = (cur && cur->kind == EX_REINTERPRET && cur->as.reinterpret_.expr)
                        ? cur->as.reinterpret_.expr->type
                        : (cur ? cur->type : emit_type_from_kind(TY_UNKNOWN));
                    /* CS3: When emitting a specialized body (current_abi_specialization is
                     * set), resolve any type variables in the arg type against the outer
                     * specialization's bindings.  This lets inner calls like pair_second(p)
                     * match the correct specialization even though p still has a generic
                     * TY_TYVAR type in the original AST. */
                    if (ctx->current_abi_specialization) {
                        actual = emit_resolve_type(ctx, actual);
                    }
                    if (!type_eq(spec->arg_types[ai], actual)) {
                        args_match = false;
                        break;
                    }
                }
                if (args_match) {
                    char *name = strdup(spec->clone_name);
                    if (!name) { fprintf(stderr, "tur: oom\n"); abort(); }
                    return name;
                }
            }
        }
    }
    /* A captured carrier (e.g. a `:fn` poly closure threaded into a returned
     * closure) is reached through its env, not by its bare local name -- the
     * poly-fn dispatch path emits `<name>.fn(<name>.env, ...)`, so `<name>`
     * here must be the env-qualified access. */
    if (b) {
        char *captured = capture_env_access(ctx, b);
        if (captured) return captured;
    }
    /* ascribed-fn-param-call-head-name-mismatch: a callee that is a LOCAL
     * binding names a C local variable, not a linker symbol, so it must be
     * spelled by the same rule that DECLARED it -- name_for_binding -- or the
     * two ends disagree and cc rejects the undeclared one.
     *
     * raw_name_for_binding is right for a global: a top-level defn's C name IS
     * the function symbol, un-suffixed. For a local it is right only by
     * coincidence, when name_for_binding happens to delegate to it too (a
     * parameter, an inline-C-named local, a non-boxed TY_FN). A synthetic
     * call-head temp that is none of those falls through name_for_binding to
     * the id-suffixed mangler, and the mismatch is a hard build break: the
     * `__call_head_N` hoisted for `((:: f (fn [int] int)) v)` -- TY_PTR_VOID +
     * is_poly_fn -- was declared `_un_uncall_unhead_unN_M` and used as
     * `__call_head_N`.
     *
     * Routing locals through name_for_binding closes that by construction,
     * present and future, and changes no name that already agreed: for every
     * local shape where name_for_binding delegates to raw_name_for_binding it
     * returns the identical string. Globals keep the raw symbol.
     *
     * Deliberately NOT fixed by making the declaration spell `__call_head_N`
     * verbatim: the "__-prefixed pure C identifier" class also contains
     * macro-template names that carry no gensym counter (`__v` / `__vw` from
     * stdlib/vec.tur's `vec-of`), whose distinctness today comes precisely from
     * the `_<id>` suffix. Dropping it for the whole class would let two nested
     * expansions collide in one C scope. */
    if (b && !b->is_global) {
        return name_for_binding(ctx, b);
    }
    return raw_name_for_binding(b);
}

/* Return a sanitized C identifier for a Binding. If the binding is a
 * function parameter in the current context, use the raw name (without ID).
 * Otherwise, append the ID suffix. Caller frees. */
/* If `b` is a captured binding in the current closure/defer/handle/generator
 * scope, return a malloc'd env-qualified access string ("<env>->field" or
 * "<env>.field"); otherwise NULL.  Shared by name_for_binding (value access)
 * and emit_call_name (callee access) so that a captured carrier called via the
 * poly-fn dispatch path is reached through its env, not by its bare name. */
static char *capture_env_access(EmitCtx *ctx, const Binding *b) {
    const char *env = NULL;
    const char *sep = "->";
    /* GF1: inside a generator _next function, struct fields live on __g->field. */
    if (ctx->gen_var_name && ctx->gen_struct_bindings) {
        for (uint32_t i = 0; i < ctx->n_gen_struct_bindings; i++) {
            if (ctx->gen_struct_bindings[i] == b) { env = ctx->gen_var_name; break; }
        }
    }
    /* Phase 3: captured binding in a closure thunk -> env_var_name->field. */
    if (!env && ctx->closure && ctx->env_var_name) {
        for (uint8_t i = 0; i < ctx->closure->n_captures; i++) {
            if (ctx->closure->captures[i] == b) {
                /* Edge 1: an eagerly-captured letrec member that resolved to a
                 * captureless global has no env field (see emit_expr.c); let it
                 * fall through to its C symbol rather than a bogus env slot. */
                if (b->is_global) break;
                env = ctx->env_var_name; break;
            }
        }
    }
    /* Phase 4 v1: captured binding in a defer thunk -> env_var_name->field. */
    if (!env && ctx->env_var_name && ctx->defer_captures) {
        for (uint8_t i = 0; i < ctx->n_defer_captures; i++) {
            if (ctx->defer_captures[i] == b) { env = ctx->env_var_name; break; }
        }
    }
    /* Phase 19D: captured binding in a handle body fiber function -> __env->field. */
    if (!env && ctx->handle_captures && ctx->handle_env_name) {
        for (uint32_t i = 0; i < ctx->n_handle_captures; i++) {
            if (ctx->handle_captures[i] == b) { env = ctx->handle_env_name; break; }
        }
    }
    if (!env) return NULL;
    char *field_name = raw_name_for_binding(b);
    size_t sz = strlen(env) + strlen(sep) + strlen(field_name) + 1;
    char *result = (char *)malloc(sz);
    if (!result) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(result, sz, "%s%s%s", env, sep, field_name);
    free(field_name);
    return result;
}

char *name_for_binding(EmitCtx *ctx, const Binding *b) {
    char *captured = capture_env_access(ctx, b);
    if (captured) return captured;
    /* van-laarhoven-lens-composition (Gap B2): the synthetic ambient-dict binding.
     * If captured (inside an adapter lambda) the check above already returned the
     * env slot.  Uncaptured, it stands for the enclosing dict-clone's dict PARAM;
     * lower to it.  In the plain carrier base (no dict param) fall back to the
     * representative instance's singleton. */
    if (b && b->is_ambient_dict) {
        if (ctx->dict_dispatch_param_cname)
            return strdup(ctx->dict_dispatch_param_cname);
        if (b->ambient_repr) {
            char dn[128];
            emit_dict_name(dn, sizeof(dn), b->ambient_repr);
            Buf ab; buf_init(&ab);
            buf_printf(&ab, "(int64_t)(intptr_t)(&%s_singleton)", dn);
            buf_putc(&ab, '\0');
            char *r = strdup(ab.data);
            buf_free(&ab);
            return r;
        }
        return strdup("0");
    }
    /* Check if this binding is a function parameter in the current context */
    if (ctx->fn_params) {
        for (uint32_t i = 0; i < ctx->n_fn_params; i++) {
            if (ctx->fn_params[i] == b) {
                return raw_name_for_binding(b);
            }
        }
    }
    /* inline-c-locals-invisible-to-inline-c-blocks: a local an inline-C block
     * in this same function names by its source spelling.  Same treatment as a
     * parameter one line above, and for the same reason -- the C text is
     * pasted verbatim, so the only name it can use is the one the author
     * wrote.  Membership is decided per function body by
     * emit_inline_c_raw_locals_collect, which admits a binding only when the
     * raw spelling is unambiguous; everything else still takes the id-suffixed
     * path below. */
    if (ctx->inline_c_raw_locals) {
        for (uint32_t i = 0; i < ctx->n_inline_c_raw_locals; i++) {
            if (ctx->inline_c_raw_locals[i] == b) {
                return raw_name_for_binding(b);
            }
        }
    }
    /* If this is a bare function reference (captureless fn / top-level defn),
     * use the raw name without ID -- its C name *is* the function symbol.
     * CRU B-1: a *boxed* TY_FN is a first-class closure *value* (a local box),
     * not a function symbol; fall through to the id-suffixed mangling path so
     * distinct closures don't collide and a source name that is a C keyword
     * (e.g. `double`) is disambiguated to `double_<id>`. */
    /* let-alias-of-fn-param-call-undeclared: a let-bound POLY-FN alias
     * (`(let [h g] ...)` with `g` a fat-normalised fn parameter) is a
     * `tur_poly_fn_t` VALUE declared with the id suffix by the let binder, not
     * a function symbol; the raw-name rule below spelled its CALL `h` against
     * a declaration `h_1608`. */
    if (b->type.kind == TY_FN && !b->type.as.fn.boxed && !b->is_poly_fn) {
        return raw_name_for_binding(b);
    }
    /* Phase M6: If c_export_name is set, use it directly (bypasses mangling and id suffix). */
    if (b->c_export_name) {
        return strdup(b->c_export_name);
    }
    /* `<name>_<id>` with non-id-safe chars mangled to underscores. We append
     * the unique id so different bindings with the same source name don't
     * collide in C. */
    size_t cap = tur_mangle_bound(b->name->len) + 16;
    char *p = (char *)malloc(cap);
    if (!p) { fprintf(stderr, "tur: oom\n"); abort(); }
    size_t k = 0;
    tur_mangle_append(p, &k, b->name->name, b->name->len);
    snprintf(p + k, cap - k, "_%u", b->id);
    return p;
}

/* Emit a C string literal from a StrSlice. */
void emit_c_string(Buf *out, StrSlice s) {
    buf_putc(out, '"');
    for (uint32_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.p[i];
        switch (c) {
            case '"':  buf_puts(out, "\\\""); break;
            case '\\': buf_puts(out, "\\\\"); break;
            case '\n': buf_puts(out, "\\n");  break;
            case '\t': buf_puts(out, "\\t");  break;
            case '\r': buf_puts(out, "\\r");  break;
            default:
                if (c < 0x20 || c == 0x7f) buf_printf(out, "\\x%02x", c);
                else                       buf_putc(out, (char)c);
        }
    }
    buf_putc(out, '"');
}

/* ------------ atomic emitters (no statements emitted) ------------ */

char *atom_nil(void)         { return strdup("((void)0)"); }
char *atom_bool(bool b)      { return strdup(b ? "true" : "false"); }
/* Phase N: emit integer literal with correct C macro for fixed-width type */
char *atom_int_typed(int64_t i, TypeKind k) {
    char buf[64];
    /* INT64_MIN has no positive spelling: `INT64_C(-9223372036854775808)`
     * expands to a negated 9223372036854775808, which does not fit a signed
     * type, so clang reads it as unsigned and warns
     * (-Wimplicitly-unsigned-literal) on every program that mentions it.
     * Spell it the way <stdint.h> does. */
    bool spells_int64 = (k != TY_INT8  && k != TY_INT16  && k != TY_INT32 &&
                         k != TY_UINT8 && k != TY_UINT16 && k != TY_UINT32 &&
                         k != TY_UINT64);
    if (i == INT64_MIN && spells_int64)
        return strdup("(-INT64_C(9223372036854775807) - 1)");
    switch (k) {
        case TY_INT8:   snprintf(buf, sizeof buf, "INT8_C(%lld)",   (long long)i); break;
        case TY_INT16:  snprintf(buf, sizeof buf, "INT16_C(%lld)",  (long long)i); break;
        case TY_INT32:  snprintf(buf, sizeof buf, "INT32_C(%lld)",  (long long)i); break;
        case TY_INT64:  snprintf(buf, sizeof buf, "INT64_C(%lld)",  (long long)i); break;
        case TY_UINT8:  snprintf(buf, sizeof buf, "UINT8_C(%llu)",  (unsigned long long)(uint64_t)i); break;
        case TY_UINT16: snprintf(buf, sizeof buf, "UINT16_C(%llu)", (unsigned long long)(uint64_t)i); break;
        case TY_UINT32: snprintf(buf, sizeof buf, "UINT32_C(%llu)", (unsigned long long)(uint64_t)i); break;
        case TY_UINT64: snprintf(buf, sizeof buf, "UINT64_C(%llu)", (unsigned long long)(uint64_t)i); break;
        default:        snprintf(buf, sizeof buf, "INT64_C(%lld)",  (long long)i); break;
    }
    return strdup(buf);
}
/* Phase N: emit float32 literal as a (float) cast */
/* R10: an infinity or a NaN has no decimal literal -- `%g` spells it `inf`
 * / `nan`, which is an undeclared identifier to cc.  `+inf.0` in a Scheme
 * (or any) source compiled to exactly that. */
static const char *atom_float_nonfinite(double f) {
    if (f != f) return "(__builtin_nan(\"\"))";
    if (f > 0 && f - f != f - f) return "(__builtin_inf())";
    if (f < 0 && f - f != f - f) return "(-__builtin_inf())";
    return NULL;
}
char *atom_float32(double f) {
    char buf[80];
    const char *nf = atom_float_nonfinite(f);
    if (nf) { snprintf(buf, sizeof buf, "((float)%s)", nf); return strdup(buf); }
    snprintf(buf, sizeof buf, "((float)%.9g)", f);
    return strdup(buf);
}

char *atom_float(double f) {
    char buf[64];
    /* The shortest of %.15g / %.16g / %.17g that reads back to the same
     * double.  A flat %.15g dropped the 16th and 17th digits, so a literal
     * like 3.141592653589793 compiled as 3.14159265358979 -- a different
     * double, where the interpreter (which keeps the parsed value) had the
     * right one.  Found by r7rs-lang-plan R7; 15 digits suffice for the
     * common literal, so most emitted C is unchanged. */
    const char *nf = atom_float_nonfinite(f);
    if (nf) return strdup(nf);
    snprintf(buf, sizeof buf, "%.15g", f);
    if (f == f && strtod(buf, NULL) != f) snprintf(buf, sizeof buf, "%.16g", f);
    if (f == f && strtod(buf, NULL) != f) snprintf(buf, sizeof buf, "%.17g", f);
    /* Ensure it's a double literal by appending .0 if needed */
    char *p = strchr(buf, '.');
    char *e = strchr(buf, 'e');
    if (!p && !e) {
        /* No decimal point or exponent - append .0 */
        strcat(buf, ".0");
    }
    return strdup(buf);
}

/* Phase N: type-dispatched float literal emitter */
static char *atom_float_typed(TypeKind k, double f) __attribute__((unused));
static char *atom_float_typed(TypeKind k, double f) {
    if (k == TY_FLOAT32) {
        char buf[64];
        snprintf(buf, sizeof buf, "(float)(%.7g)", f);
        return strdup(buf);
    }
    return atom_float(f);
}
/* GHE2: when emitting inside a monomorphized constrained generic, a reference to
 * a *generic function as a value* (e.g. a comparator fn passed to map-assoc-eq)
 * must name the per-K child clone that the abi scan created under the active
 * specialization's bindings -- not the carrier base clone whose body bakes the
 * Eq[int] instance.  Mirrors emit_reresolve_method_call (which handles method
 * *calls*) but for fn *values*.  Returns the clone name, or NULL when no child
 * spec applies (caller falls back to name_for_binding). */
static char *emit_reresolve_fn_value(EmitCtx *ctx, const Binding *b) {
    const EmitAbiSpecialization *outer = ctx ? ctx->current_abi_specialization : NULL;
    if (!outer || !b || b->type.kind != TY_FN) return NULL;
    for (uint32_t i = 0; i < ctx->n_abi_specializations; i++) {
        const EmitAbiSpecialization *spec = &ctx->abi_specializations[i];
        if (spec->binding != b || !spec->clone_name) continue;
        /* The child spec was interned under the same tyvar->type bindings the
         * outer spec carries, so it matches iff those bindings agree. */
        if (spec->n_bindings != outer->n_bindings) continue;
        bool ok = true;
        for (uint8_t bi = 0; bi < spec->n_bindings; bi++) {
            const AbiTypeBinding *sb = &spec->bindings[bi];
            const AbiTypeBinding *ob = &outer->bindings[bi];
            if (!sb->name || !ob->name || strcmp(sb->name, ob->name) != 0 ||
                !type_eq(sb->type, ob->type)) { ok = false; break; }
        }
        if (!ok) continue;
        char *name = strdup(spec->clone_name);
        if (!name) { fprintf(stderr, "tur: oom\n"); abort(); }
        return name;
    }
    return NULL;
}

char *atom_var(EmitCtx *ctx, const Binding *b) {
    char *reresolved = emit_reresolve_fn_value(ctx, b);
    if (reresolved) return reresolved;
    char *nm = name_for_binding(ctx, b);
    /* G4a (mutable-globals-plan §4.4): a read of an `^atomic` global is a
     * sequentially-consistent load, not a bare reference.  This is the
     * value-position chokepoint -- `name_for_binding` itself must stay bare,
     * because it also spells the DEFINITION (`static int64_t g;`) and every
     * assignment target.
     *
     * Wrapping the read matters as much as the write: a bare `g` in a loop is
     * free to be hoisted into a register, so a thread would never observe
     * another's store no matter how atomically that store was made.
     *
     * The macro layer takes a pointer, so this works under the JIT unchanged --
     * atomicity is an operation on storage the JIT owns, where thread-local
     * storage would be storage the host has to own. */
    /* G4b: a read of a `^thread-local` goes through its accessor, which
     * materializes this thread's block and runs the initializer on first
     * touch.  Same chokepoint as `^atomic`, for the same reason. */
    if (b && b->is_thread_local) {
        size_t n = strlen(nm) + 32;
        char *out = (char *)malloc(n);
        if (!out) { fprintf(stderr, "tur: oom\n"); abort(); }
        snprintf(out, n, "__tur_tl_get_%s()", nm);
        free(nm);
        return out;
    }
    if (b && b->is_atomic) {
        const char *ld = (b->type.kind == TY_PTR_VOID || b->type.kind == TY_CSTR)
                       ? "TUR_ATOMIC_LOAD_PTR" : "TUR_ATOMIC_LOAD_U64";
        size_t n = strlen(nm) + 96;
        char *out = (char *)malloc(n);
        if (!out) { fprintf(stderr, "tur: oom\n"); abort(); }
        if (b->type.kind == TY_FLOAT) {
            /* A double is loaded through its bit pattern: the macro layer's
             * host shim is integer-typed, and reinterpreting keeps one code
             * path for both front ends instead of a double-typed shim only the
             * GNU branch could use. */
            snprintf(out, n,
                     "__tur_bits_to_f64(TUR_ATOMIC_LOAD_U64((const volatile uint64_t *)&%s, __ATOMIC_SEQ_CST))",
                     nm);
        } else if (b->type.kind == TY_PTR_VOID || b->type.kind == TY_CSTR) {
            snprintf(out, n, "(%s)%s((void *const volatile *)&%s, __ATOMIC_SEQ_CST)",
                     type_c_name(b->type), ld, nm);
        } else {
            snprintf(out, n, "(%s)TUR_ATOMIC_LOAD_U64((const volatile uint64_t *)&%s, __ATOMIC_SEQ_CST)",
                     type_c_name(b->type), nm);
        }
        free(nm);
        return out;
    }
    /* B7b: a `^mut` promoted to a SHARED HEAP CELL (the CPS backend does this for
     * a mutable a lifted body touches -- a handler clause, which is emitted as
     * its own C function and cannot see the enclosing frame's locals; see
     * emit_cps_ir.c's g_byref_muts).  Its C name binds the cell POINTER, so a
     * value-position read derefs.  Same chokepoint, and for the same reason as
     * the two above: `name_for_binding` must stay bare because it also spells
     * the declaration and every assignment target. */
    if (emit_binding_is_byref_cell(b)) {
        size_t n = strlen(nm) + 8;
        char *out = (char *)malloc(n);
        if (!out) { fprintf(stderr, "tur: oom\n"); abort(); }
        snprintf(out, n, "(*%s)", nm);
        free(nm);
        return out;
    }
    return nm;
}
char *atom_cstr(StrSlice s) {
    /* Build into a Buf, then strdup out. */
    Buf b; buf_init(&b);
    emit_c_string(&b, s);
    buf_putc(&b, '\0');
    char *p = strdup(b.data);
    buf_free(&b);
    return p;
}

/* Phase G: scan an inline-C body for __TUR_TY_<NAME>__ template markers.
 * Used by emit_abi_register_call to decide whether an inline-C function
 * opts in to ABI specialization. */
bool inline_c_has_ty_template(const InlineC *ic) {
    if (!ic || !ic->code.p) return false;
    const char *code = ic->code.p;
    uint32_t len = ic->code.len;
    if (len < 10) return false;
    for (uint32_t i = 0; i + 9 < len; i++) {
        if (memcmp(code + i, "__TUR_TY_", 9) == 0) return true;
    }
    /* end-to-end-monomorphization: `__TUR_RET__` (the active spec's concrete
     * result C name, or int64_t for the carrier base) makes the body ABI-
     * dependent the same way `__TUR_TY_<NAME>__` does -- a `:heap` producer
     * uses it for its return cast (`return (__TUR_RET__)(intptr_t)v;`). */
    if (len >= 11) {
        for (uint32_t i = 0; i + 11 <= len; i++) {
            if (memcmp(code + i, "__TUR_RET__", 11) == 0) return true;
        }
    }
    return false;
}

bool inline_c_has_cname_template(const InlineC *ic) {
    if (!ic || !ic->code.p) return false;
    const char *code = ic->code.p;
    uint32_t len = ic->code.len;
    if (len < 12) return false;
    for (uint32_t i = 0; i + 12 <= len; i++) {
        if (memcmp(code + i, "__TUR_CNAME_", 12) == 0) return true;
    }
    return false;
}

/* inline-c-function-scope-include-guards fix: scan the start of `body` for
 * `#include <...>` / `#include "..."` directives, strip them from the body,
 * and add each one (deduped) to ctx->hoisted_includes. The emitter later
 * emits the collected directives at file scope so include-guarded headers
 * (e.g. sqlite3.h) are visible to every inline-C function in the TU --
 * function-scope #include is otherwise hidden from later functions by the
 * header's own include guard.
 *
 * Conservative scanner: only consumes leading lines made entirely of
 * whitespace, comments (// and / *...* /), or `#include <foo>` /
 * `#include "foo"`. Stops at the first line that doesn't match -- so an
 * #include sitting after real C code (e.g. inside an #ifdef block guarded
 * by surrounding logic) is left in place.
 *
 * Modifies `body` in place by shifting the tail over the consumed prefix.
 */
extern char    **g_hoisted_includes;
extern uint32_t  g_n_hoisted_includes;
extern uint32_t  g_cap_hoisted_includes;

/* Does the stored hoisted-include entry carry the `tur:optional` marking? */
static bool hoisted_entry_is_optional(const char *e) {
    size_t el = strlen(e), tl = sizeof(TUR_HOIST_OPTIONAL_TAG) - 1;
    return el >= tl && memcmp(e + el - tl, TUR_HOIST_OPTIONAL_TAG, tl) == 0;
}

/* Length of the directive itself, with any `tur:optional` marking removed.
 * This is the dedup key, so a header written bare at one site and marked at
 * another is one entry, not two. */
static size_t hoisted_entry_key_len(const char *e) {
    size_t el = strlen(e);
    return hoisted_entry_is_optional(e) ? el - (sizeof(TUR_HOIST_OPTIONAL_TAG) - 1)
                                        : el;
}

/* Emit one hoisted `#include` line, making system (angle-bracket) headers
 * fail-soft across platforms -- but never silently.
 *
 * A hoisted `#include <X>` sits at file scope with no `#ifdef` around it, so a
 * header that exists on one platform but not another would hard-fail the build
 * on the platform that lacks it. Wrapping angle includes in __has_include skips
 * a missing system header instead. This keeps the emitted C portable, which is
 * the WIN1 contract, and stdlib depends on it deliberately: fs/mkdir, term and
 * image each write a per-platform header (<direct.h>, <io.h>, <windows.h>,
 * <mach-o/dyld.h>) as a bare LEADING include precisely so the hoister lifts it
 * and this wrap drops it on the platforms that lack it.
 *
 * hoisted-includes-wrapped-in-has-include: the skip used to be silent, which
 * made "your -I is wrong" indistinguishable from "this header is deliberately
 * absent here". A missing header the author actually needed produced no
 * mention of the header at all -- just implicit declarations, or a link error,
 * thousands of lines away (spices/raygui pointed -I at the repo root instead of
 * src/ and read as a pile of FFI binding bugs). So an unmarked angle header
 * that is not found now announces itself by name in the `#else`. It stays a
 * `#pragma message` rather than an `#error`: the tolerant path is load-bearing
 * (12 of the 18 hoisted headers in-tree are platform-conditional), and the
 * emitted C is compiled with -Wall and never -Werror, so this can name the
 * problem without ever being the thing that breaks a build.
 *
 * A deliberate per-platform alternative opts out with a `tur:optional` comment
 * on the include line, which is what the stdlib sites above carry.
 *
 * Quoted `#include "X"` is left bare: those are project/vendored headers where
 * "missing" is a genuine error and should stay a loud "no such file", not a
 * silent skip. __has_include is defined by every toolchain that compiles this
 * code (GCC 5+, Clang, MSVC 2017+, Emscripten); so is `#pragma message`. */
void tur_emit_hoisted_include(Buf *out, const char *line) {
    bool optional = hoisted_entry_is_optional(line);
    size_t key = hoisted_entry_key_len(line);
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    /* An angle include has '<' after "#include"; a quoted one has '"'.  Only
     * look inside the directive itself, never into the `tur:optional` marking. */
    const char *end = line + key;
    const char *lt = (const char *)memchr(p, '<', (size_t)(end - p));
    const char *qt = (const char *)memchr(p, '"', (size_t)(end - p));
    int is_angle = (lt != NULL && (qt == NULL || lt < qt));
    if (is_angle) {
        const char *gt = (const char *)memchr(lt, '>', (size_t)(end - lt));
        if (gt) {
            buf_puts(out, "#if __has_include(");
            /* the exact <...> spec */
            for (const char *c = lt; c <= gt; c++) buf_putc(out, *c);
            buf_puts(out, ")\n");
            for (const char *c = line; c < end; c++) buf_putc(out, *c);
            buf_puts(out, "\n#else\n");
            if (optional) {
                buf_puts(out, "/* tur: optional per-platform header; skipped when absent */\n");
            } else {
                buf_puts(out, "#pragma message(\"tur: inline-C requested ");
                for (const char *c = lt; c <= gt; c++) buf_putc(out, *c);
                buf_puts(out, ", not found on the include path -- code needing it "
                              "will fail to compile or link; add -I, or mark the "
                              "include tur:optional if it is a deliberate "
                              "per-platform alternative\")\n");
            }
            buf_puts(out, "#endif\n");
            return;
        }
    }
    for (const char *c = line; c < end; c++) buf_putc(out, *c);
    buf_puts(out, "\n");
}

void tur_hoist_include_add(const char *line, size_t n) {
    tur_hoist_include_add_ex(line, n, false);
}

void tur_hoist_include_add_ex(const char *line, size_t n, bool optional) {
    /* Trim trailing whitespace/CR. */
    while (n > 0 && (line[n-1] == ' ' || line[n-1] == '\t' || line[n-1] == '\r'))
        n--;
    if (n == 0) return;
    for (uint32_t i = 0; i < g_n_hoisted_includes; i++) {
        char *e = g_hoisted_includes[i];
        if (hoisted_entry_key_len(e) != n || memcmp(e, line, n) != 0) continue;
        /* Same directive.  If this site marks it optional and the stored entry
         * is bare, upgrade the entry: one deliberate per-platform use is enough
         * to make the header's absence expected for the whole TU. */
        if (g_hoist_origin_stdlib) g_hoisted_stdlib[i] = true;
        if (optional && !hoisted_entry_is_optional(e)) {
            size_t tl = sizeof(TUR_HOIST_OPTIONAL_TAG) - 1;
            char *up = (char *)malloc(n + tl + 1);
            if (!up) { fprintf(stderr, "tur: oom\n"); abort(); }
            memcpy(up, line, n);
            memcpy(up + n, TUR_HOIST_OPTIONAL_TAG, tl + 1);
            free(e);
            g_hoisted_includes[i] = up;
        }
        return; /* dup */
    }
    if (g_n_hoisted_includes == g_cap_hoisted_includes) {
        uint32_t cap = g_cap_hoisted_includes ? g_cap_hoisted_includes * 2 : 8;
        char **nh = (char **)realloc(g_hoisted_includes, cap * sizeof(char *));
        bool *ns = (bool *)realloc(g_hoisted_stdlib, cap * sizeof(bool));
        if (!nh || !ns) { fprintf(stderr, "tur: oom\n"); abort(); }
        g_hoisted_includes = nh;
        g_hoisted_stdlib = ns;
        g_cap_hoisted_includes = cap;
    }
    size_t tl = optional ? sizeof(TUR_HOIST_OPTIONAL_TAG) - 1 : 0;
    char *copy = (char *)malloc(n + tl + 1);
    if (!copy) { fprintf(stderr, "tur: oom\n"); abort(); }
    memcpy(copy, line, n);
    if (optional) memcpy(copy + n, TUR_HOIST_OPTIONAL_TAG, tl);
    copy[n + tl] = '\0';
    g_hoisted_stdlib[g_n_hoisted_includes] = g_hoist_origin_stdlib;
    g_hoisted_includes[g_n_hoisted_includes++] = copy;
}

/* Does the remainder of an `#include` line (everything past the closing
 * delimiter) carry a `tur:optional` marking?  Written as a comment, e.g.
 *   #include <direct.h>  / * tur:optional * /
 * It marks a header whose absence is EXPECTED on some target -- a deliberate
 * per-platform alternative -- so the __has_include skip stays silent instead
 * of announcing itself.  See tur_emit_hoisted_include. */
static bool hoist_rest_marks_optional(const char *p, size_t n) {
    static const char needle[] = "tur:optional";
    size_t nl = sizeof needle - 1;
    if (n < nl) return false;
    for (size_t i = 0; i + nl <= n; i++)
        if (memcmp(p + i, needle, nl) == 0) return true;
    return false;
}

/* Returns the count of bytes consumed at the start of `body` (the prefix
 * containing only blank lines, comments, and #include directives that were
 * hoisted). Caller should `memmove(body, body+consumed, ...)` to drop them. */
size_t tur_hoist_top_includes_scan(const char *body, size_t len) {
    size_t i = 0;
    size_t last_consumed = 0;
    while (i < len) {
        /* Find end of current line. */
        size_t line_start = i;
        while (i < len && body[i] != '\n') i++;
        size_t line_end = i;
        if (i < len) i++; /* consume '\n' */
        size_t consumed_to = i;
        size_t line_len = line_end - line_start;
        const char *L = body + line_start;
        /* Skip leading whitespace. */
        size_t k = 0;
        while (k < line_len && (L[k] == ' ' || L[k] == '\t')) k++;
        /* Blank line? continue, count as part of hoistable prefix. */
        if (k == line_len) { last_consumed = consumed_to; continue; }
        /* `// ...` line comment? skip. */
        if (k + 1 < line_len && L[k] == '/' && L[k+1] == '/') {
            last_consumed = consumed_to;
            continue;
        }
        /* `#include ...` directive? */
        if (k + 8 <= line_len && L[k] == '#' &&
            memcmp(L + k + 1, "include", 7) == 0 &&
            (k + 8 == line_len || L[k+8] == ' ' || L[k+8] == '\t')) {
            /* Validate that the rest of the line is `<...>` or `"..."` plus
             * optional trailing whitespace/comment. */
            size_t j = k + 8;
            while (j < line_len && (L[j] == ' ' || L[j] == '\t')) j++;
            if (j < line_len && (L[j] == '<' || L[j] == '"')) {
                char close = (L[j] == '<') ? '>' : '"';
                size_t end = j + 1;
                while (end < line_len && L[end] != close) end++;
                if (end < line_len && L[end] == close) {
                    /* OK; record `#include <...>` (from k to end+1).  Anything
                     * after the closing delimiter is a trailing comment; it is
                     * not stored, but a `tur:optional` marking in it is. */
                    bool opt = hoist_rest_marks_optional(L + end + 1,
                                                         line_len - (end + 1));
                    tur_hoist_include_add_ex(L + k, (end + 1) - k, opt);
                    last_consumed = consumed_to;
                    continue;
                }
            }
            /* Malformed include — stop scanning, leave it for the C compiler. */
            break;
        }
        /* `#define <NAME> <VALUE?>` directive? Hoist object-like macros so
         * the file-scope #include they configure (e.g. `#define
         * PCRE2_CODE_UNIT_WIDTH 8` before `#include <pcre2.h>`) takes effect
         * once at file scope rather than per-function -- mirroring the
         * include-guards issue this scanner exists to fix.  Conservative:
         * only `#define IDENT [rest]` with no function-like `(` between the
         * identifier and the value; `#define FOO(X) ...` macros stay in
         * place. */
        if (k + 8 <= line_len && L[k] == '#' &&
            memcmp(L + k + 1, "define", 6) == 0 &&
            (k + 7 == line_len || L[k+7] == ' ' || L[k+7] == '\t')) {
            size_t j = k + 7;
            while (j < line_len && (L[j] == ' ' || L[j] == '\t')) j++;
            size_t id_start = j;
            while (j < line_len &&
                   ((L[j] >= 'A' && L[j] <= 'Z') ||
                    (L[j] >= 'a' && L[j] <= 'z') ||
                    (L[j] >= '0' && L[j] <= '9') ||
                    L[j] == '_')) j++;
            if (j > id_start &&
                /* Not a function-like macro: skip if next char (no
                 * whitespace) is '('. */
                !(j < line_len && L[j] == '(')) {
                /* Whole line is the macro definition. */
                tur_hoist_include_add(L + k, line_len - k);
                last_consumed = consumed_to;
                continue;
            }
            /* Function-like macro or malformed: stop scanning. */
            break;
        }
        /* Anything else: stop. */
        break;
    }
    return last_consumed;
}

static char *strip_hoistable_includes(EmitCtx *ctx, char *body, bool from_stdlib) {
    (void)ctx;
    if (!body) return body;
    size_t len = strlen(body);
    g_hoist_origin_stdlib = from_stdlib;
    size_t consumed = tur_hoist_top_includes_scan(body, len);
    g_hoist_origin_stdlib = false;
    if (consumed > 0) {
        memmove(body, body + consumed, len - consumed + 1);
    }
    return body;
}

/* SS2: Perform __TUR_CAP_N__ / __TUR_VAL_N__ substitution on an InlineC node.
 * Emits val_exprs[N] into temp vars in body, then returns a malloc'd C string
 * with all __TUR_CAP_N__ and __TUR_VAL_N__ placeholders substituted.
 *
 * Phase G: also substitutes __TUR_TY_<NAME>__ placeholders. Under an active
 * ABI specialization, <NAME> resolves to the concrete C type bound to that
 * type variable; otherwise (generic / carrier emission) it resolves to
 * int64_t (the carrier). val_expr temporary types are also resolved through
 * the active specialization, so __TUR_VAL_N__ temps land at the concrete
 * C width rather than the generic carrier width. */
char *inline_c_substitute(EmitCtx *ctx, Buf *body, InlineC *ic) {
    bool has_ty_template = inline_c_has_ty_template(ic);
    bool has_cname_template = inline_c_has_cname_template(ic);

    /* Fast path: no substitution needed. */
    if (ic->n_captures == 0 && ic->n_val_exprs == 0 && !has_ty_template &&
        !has_cname_template) {
        return strip_hoistable_includes(ctx, strndup(ic->code.p, ic->code.len),
                                        ic->from_stdlib);
    }

    /* Build capture name array. */
    char **cap_names = NULL;
    if (ic->n_captures > 0) {
        cap_names = (char **)calloc(ic->n_captures, sizeof(char *));
        for (int ci = 0; ci < ic->n_captures; ci++) {
            cap_names[ci] = name_for_binding(ctx, ic->captures[ci]);
        }
    }

    /* Evaluate val_exprs into temp variables. Phase G: resolve the temp
     * declared type through the active specialization so a TY_TYVAR-typed
     * subexpression ends up at the concrete C width. */
    char **val_temps = NULL;
    if (ic->n_val_exprs > 0) {
        val_temps = (char **)calloc(ic->n_val_exprs, sizeof(char *));
        for (int vi = 0; vi < ic->n_val_exprs; vi++) {
            char *tmp = fresh_tmp(ctx);
            char *vv = emit_value(ctx, body, ic->val_exprs[vi]);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s = %s;\n",
                       emit_type_c_name(ctx, ic->val_exprs[vi]->type), tmp, vv);
            free(vv);
            val_temps[vi] = tmp;
        }
    }

    /* Scan the code string and perform substitution. */
    const char *code = ic->code.p;
    uint32_t len = ic->code.len;
    Buf result; buf_init(&result);
    for (uint32_t i = 0; i < len; ) {
        bool matched = false;
        /* Check for __TUR_CAP_N__ */
        if (i + 12 <= len && memcmp(code + i, "__TUR_CAP_", 10) == 0) {
            uint32_t j = i + 10;
            int n = 0; bool have_digit = false;
            while (j < len && code[j] >= '0' && code[j] <= '9') {
                n = n * 10 + (code[j] - '0'); j++; have_digit = true;
            }
            if (have_digit && j + 1 < len && code[j] == '_' && code[j+1] == '_') {
                j += 2;
                buf_puts(&result, (cap_names && n < ic->n_captures) ? cap_names[n] : "NULL");
                i = j; matched = true;
            }
        }
        /* Check for __TUR_VAL_N__ */
        if (!matched && i + 12 <= len && memcmp(code + i, "__TUR_VAL_", 10) == 0) {
            uint32_t j = i + 10;
            int n = 0; bool have_digit = false;
            while (j < len && code[j] >= '0' && code[j] <= '9') {
                n = n * 10 + (code[j] - '0'); j++; have_digit = true;
            }
            if (have_digit && j + 1 < len && code[j] == '_' && code[j+1] == '_') {
                j += 2;
                buf_puts(&result, (val_temps && n < ic->n_val_exprs) ? val_temps[n] : "0");
                i = j; matched = true;
            }
        }
        /* Phase G: Check for __TUR_TY_<NAME>__ — substitutes to the concrete
         * C type bound to <NAME> in the active ABI specialization, or to
         * int64_t (the carrier) when no specialization is active. */
        if (!matched && i + 11 <= len && memcmp(code + i, "__TUR_TY_", 9) == 0) {
            uint32_t j = i + 9;
            uint32_t name_start = j;
            while (j < len && (
                (code[j] >= 'A' && code[j] <= 'Z') ||
                (code[j] >= 'a' && code[j] <= 'z') ||
                (code[j] >= '0' && code[j] <= '9') ||
                code[j] == '_')) {
                /* a single trailing "__" closes the placeholder; check that
                 * we have not stepped into the closing pair before consuming */
                if (code[j] == '_' && j + 1 < len && code[j+1] == '_') {
                    /* peek: only treat as terminator if this leaves a real
                     * name (j > name_start) and the chars after are not
                     * additional underscores (a name like FOO__BAR is allowed
                     * by greedy match below) */
                    break;
                }
                j++;
            }
            if (j > name_start && j + 1 < len && code[j] == '_' && code[j+1] == '_') {
                uint32_t name_len = j - name_start;
                char name_buf[64];
                if (name_len < sizeof(name_buf)) {
                    memcpy(name_buf, code + name_start, name_len);
                    name_buf[name_len] = '\0';
                    const char *resolved = "int64_t";
                    const EmitAbiSpecialization *spec =
                        ctx ? ctx->current_abi_specialization : NULL;
                    if (spec) {
                        for (uint8_t bi = 0; bi < spec->n_bindings; bi++) {
                            if (spec->bindings[bi].name &&
                                strcmp(spec->bindings[bi].name, name_buf) == 0) {
                                resolved = type_c_name(spec->bindings[bi].type);
                                break;
                            }
                        }
                    }
                    buf_puts(&result, resolved);
                    i = j + 2; matched = true;
                }
            }
        }
        /* end-to-end-monomorphization: `__TUR_RET__` expands to the active ABI
         * spec's concrete result C name (e.g. `Vec__int *` for `vec-new`'s
         * `(Vec int)` spec), or `int64_t` for the carrier base when no spec is
         * active.  Lets a `:heap` producer's inline-C body cast its return to
         * the right type for both ABIs from one body:
         *   `return (__TUR_RET__)(intptr_t)v;`
         * -> `return (int64_t)(intptr_t)v;`   (carrier base)
         * -> `return (Vec__int *)(intptr_t)v;` (typed spec). */
        if (!matched && i + 11 <= len && memcmp(code + i, "__TUR_RET__", 11) == 0) {
            const char *resolved = "int64_t";
            const EmitAbiSpecialization *spec =
                ctx ? ctx->current_abi_specialization : NULL;
            if (spec) {
                resolved = type_c_name(spec->result_type);
            } else if (ctx && ctx->current_fn_ret_ctype) {
                resolved = ctx->current_fn_ret_ctype;
            }
            buf_puts(&result, resolved);
            i += 11; matched = true;
        }
        /* Name-reference splice: __TUR_CNAME_<source-name>__ expands to the
         * mangled C identifier of <source-name>, routed through the same
         * tur_mangle_append helper the emitter uses for binding names. This
         * lets inline-C call a sibling defn without hardcoding the mangled
         * spelling (e.g. write __TUR_CNAME_tur-int-carrier-eq?__ rather than
         * the literal tur_int_carrier_eq_qu). <source-name> may contain sigils
         * (-, ?, !, =, ...); it is terminated by the first "__" after the
         * prefix. The result carries no module prefix, matching the unprefixed
         * C names of module-local stdlib globals. */
        if (!matched && i + 14 <= len && memcmp(code + i, "__TUR_CNAME_", 12) == 0) {
            uint32_t name_start = i + 12;
            uint32_t name_len = tur_cname_name_len(code, len, name_start);
            if (name_len > 0) {
                size_t k = 0;
                char *mangled = (char *)malloc(tur_mangle_bound(name_len) + 1);
                if (!mangled) { fprintf(stderr, "tur: oom\n"); abort(); }
                tur_mangle_append(mangled, &k, code + name_start, name_len);
                mangled[k] = '\0';
                buf_puts(&result, mangled);
                free(mangled);
                i = name_start + name_len + 2; matched = true;
            }
        }
        if (!matched) { buf_putc(&result, code[i++]); }
    }

    /* Free temporaries. */
    for (int ci = 0; ci < ic->n_captures; ci++) free(cap_names[ci]);
    free(cap_names);
    for (int vi = 0; vi < ic->n_val_exprs; vi++) free(val_temps[vi]);
    free(val_temps);

    buf_putc(&result, '\0');
    char *out = strdup(result.data);
    buf_free(&result);
    return strip_hoistable_includes(ctx, out, ic->from_stdlib);
}

/* ------------ builtin emitters ------------ */

char *emit_builtin(EmitCtx *ctx, Buf *body, const Expr *e) {
    const BuiltinSpec *spec = e->as.builtin.spec;
    uint32_t n = e->as.builtin.n;
    Expr **args = e->as.builtin.args;

    /* Eagerly emit all args (left-to-right) for non-short-circuit ops. */
    if (spec->shape == BS_AND_SC || spec->shape == BS_OR_SC) {
        /* Short-circuit: evaluate left, then conditionally evaluate the rest.
         * Lower (and a b c) to: bool t = a; if (t) t = b; if (t) t = c;
         * Lower (or  a b c) to: bool t = a; if (!t) t = b; if (!t) t = c; */
        char *tmp = fresh_tmp(ctx);
        char *first = emit_value(ctx, body, args[0]);
        indent_buf(body, ctx->indent);
        buf_printf(body, "bool %s = %s;\n", tmp, first);
        free(first);
        for (uint32_t i = 1; i < n; i++) {
            indent_buf(body, ctx->indent);
            buf_printf(body, "if (%s%s) {\n",
                       spec->shape == BS_OR_SC ? "!" : "", tmp);
            ctx->indent += 4;
            char *next = emit_value(ctx, body, args[i]);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s = %s;\n", tmp, next);
            free(next);
            ctx->indent -= 4;
            indent_buf(body, ctx->indent);
            buf_puts(body, "}\n");
        }
        return tmp;
    }

    /* println — emits a stmt and returns a nil placeholder. */
    if (spec->shape == BS_PRINTLN_INT ||
        spec->shape == BS_PRINTLN_FLOAT ||
        spec->shape == BS_PRINTLN_BOOL ||
        spec->shape == BS_PRINTLN_CSTR ||
        spec->shape == BS_PRINTLN_UINT ||
        spec->shape == BS_PRINTLN_FLOAT32) {
        char *arg = emit_value(ctx, body, args[0]);
        indent_buf(body, ctx->indent);
        char *stmt = builtin_print_stmt(spec, spec->shape, arg);
        buf_printf(body, "%s\n", stmt);
        free(stmt);
        free(arg);
        return atom_nil();
    }

    /* For everything else, evaluate args and build a C expression.
     * calloc so every slot starts NULL: this makes any unreached arg slot
     * well-defined (silences -Werror=maybe-uninitialized on '*arg_strs',
     * which gcc cannot prove is written when it cannot bound n>=1). */
    char **arg_strs = (char **)calloc(n ? n : 1, sizeof(char *));
    if (!arg_strs) { fprintf(stderr, "tur: oom\n"); abort(); }
    /* stdlib-list-null-check-retires-regions: `(= (:: l :int) 0)` is how a
     * typed list (or any `:heap` node) is tested for the empty link, and the
     * erasing ascription it needs notes the node as an escape -- so every
     * `with-region` that walked a stdlib list retired instead of rewinding.
     * A word compared with 0 and dropped escapes nowhere: let that one
     * ascription skip its note (EX_ASCRIBE reads the flag). */
    int32_t compare_only_arg = -1;
    if (spec->shape == BS_BIN_INFIX && n == 2 &&
        (strcmp(spec->c_op, "==") == 0 || strcmp(spec->c_op, "!=") == 0)) {
        for (uint32_t i = 0; i < 2; i++) {
            const Expr *lit = args[1 - i];
            if (args[i]->kind == EX_ASCRIBE &&
                lit->kind == EX_INT_LIT && lit->as.i == 0)
                compare_only_arg = (int32_t)i;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        ctx->region_erasure_compare_only = ((int32_t)i == compare_only_arg);
        arg_strs[i] = emit_value(ctx, body, args[i]);
        ctx->region_erasure_compare_only = false;
    }

    Buf out; buf_init(&out);
    switch (spec->shape) {
        case BS_BIN_INFIX:
            /* bit-shr is documented (stdlib/docstrings.tur) as a LOGICAL
             * (unsigned) right shift, but its int64_t operand is signed --
             * a bare ">>" is an arithmetic shift on virtually every real C
             * compiler, sign-extending instead of 0-filling. ">>" is used
             * by no other builtin (grep src/compiler/builtins.c), so this
             * cast is exact to bit-shr and cannot affect any other operator
             * sharing BS_BIN_INFIX. */
            if (strcmp(spec->c_op, ">>") == 0)
                buf_printf(&out, "(int64_t)((uint64_t)(%s) >> (uint64_t)(%s))",
                           arg_strs[0], arg_strs[1]);
            else
                buf_printf(&out, "(%s) %s (%s)",
                           arg_strs[0], spec->c_op, arg_strs[1]);
            break;
        case BS_VARIADIC_FOLD: {
            /* ((a OP b) OP c) OP d ... -- no redundant outermost wrap */
            uint32_t group_opens = (n >= 2) ? n - 2 : 0;
            for (uint32_t i = 0; i < group_opens; i++) buf_putc(&out, '(');
            buf_printf(&out, "(%s)", arg_strs[0]);
            for (uint32_t i = 1; i < n; i++) {
                buf_printf(&out, " %s (%s)", spec->c_op, arg_strs[i]);
                if (i < n - 1) buf_putc(&out, ')');
            }
            break;
        }
        case BS_DIV_CHECK:
            /* Integer division by zero is UB with no value to produce, so it
             * keeps the runtime guard. Float division by zero is defined by
             * IEEE 754 (inf / NaN) -- guarding it would abort on a legitimate
             * value, cost a branch per division, and draw
             * -Wliteral-conversion on a constant divisor. */
            if (builtin_div_is_ieee(spec)) {
                buf_printf(&out, "(%s) / (%s)", arg_strs[0], arg_strs[1]);
            } else {
                buf_printf(&out, "((%s) ? ((%s) / (%s)) : (fprintf(stderr, \"division by zero\\n\"), abort(), 0))",
                           arg_strs[1], arg_strs[0], arg_strs[1]);
            }
            break;
        case BS_PREFIX_UNARY:
            buf_printf(&out, "%s(%s)", spec->c_op, arg_strs[0]);
            break;
        case BS_PREFIX_UNARY_FREE:
            /* Special case for drop!: free the ref pointer directly
             * arg is a ref<T> which is stored as void* in C for v1
             * Emit: free(arg) as a statement
             * Unlike other builtins, this has side effects and is always emitted
             * as a statement, so we emit it directly here and return nil */
            indent_buf(body, ctx->indent);
            buf_printf(body, "free(%s);\n", arg_strs[0]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_PTR_WRITE:
            /* Special case for ptr-write: *ptr = value as a statement
             * Emit: *((int64_t *)arg0) = arg1; as a statement, return nil
             * We cast to int64_t * because all Turmeric values are int64_t in v1 */
            indent_buf(body, ctx->indent);
            buf_printf(body, "*((int64_t *)%s) = %s;\n", arg_strs[0], arg_strs[1]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_PTR_ARITH:
            /* Pointer arithmetic: (char *)ptr op offset
             * In C, pointer arithmetic requires a typed pointer, so we cast to char *
             * Emit: (char *)arg0 op arg1 */
            buf_printf(&out, "((char *)%s %s %s)", arg_strs[0], spec->c_op, arg_strs[1]);
            break;
        case BS_PTR_DEREF:
            /* Pointer dereference: *((T *)ptr)
             * c_op is the prefix like "*((int64_t *)" and we append arg + ")"
             * Emit: c_op + arg + ")" */
            buf_printf(&out, "%s%s)", spec->c_op, arg_strs[0]);
            break;
        /* Phase U3: Unsafe primitives - type casting */
        case BS_UNSAFE_CAST:
            /* unsafe-cast: C-style cast from one type to another
             * arg0 = value to cast, arg1 = type keyword (we ignore it and cast to int64_t for v1)
             * Emit: (int64_t)arg0 */
            buf_printf(&out, "((int64_t)%s)", arg_strs[0]);
            break;
        case BS_REINTERPRET:
            /* reinterpret: bitwise reinterpretation - same as unsafe-cast for v1
             * Emit: (int64_t)arg0 */
            buf_printf(&out, "((int64_t)%s)", arg_strs[0]);
            break;
        case BS_TRANSMUTE:
            /* transmute: bitwise reinterpretation; size equality verified at
             * elaboration time. Cast through the result type. */
            buf_printf(&out, "((%s)%s)", type_c_name(e->type), arg_strs[0]);
            break;
        /* Phase U3: Unsafe primitives - unchecked array ops */
        case BS_ARRAY_GET_UNCHECKED:
            /* array-get-unchecked: *(ptr + index)
             * arg0 = pointer, arg1 = index
             * Emit: *((int64_t *)arg0 + arg1) */
            buf_printf(&out, "(*((int64_t *)%s + %s))", arg_strs[0], arg_strs[1]);
            break;
        case BS_ARRAY_SET_UNCHECKED:
            /* array-set-unchecked: *(ptr + index) = value
             * arg0 = pointer, arg1 = index, arg2 = value
             * Emit as statement: *((int64_t *)arg0 + arg1) = arg2; */
            indent_buf(body, ctx->indent);
            buf_printf(body, "(*((int64_t *)%s + %s) = %s);\n", arg_strs[0], arg_strs[1], arg_strs[2]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        /* Phase U3: Unsafe primitives - raw memory */
        case BS_RAW_MALLOC:
            /* raw-malloc: malloc(size)
             * arg0 = size
             * Emit: malloc(arg0) */
            buf_printf(&out, "malloc(%s)", arg_strs[0]);
            break;
        case BS_RAW_FREE:
            /* raw-free: free(ptr)
             * arg0 = pointer
             * Emit as statement: free(arg0); */
            indent_buf(body, ctx->indent);
            buf_printf(body, "free(%s);\n", arg_strs[0]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_RAW_REALLOC:
            /* raw-realloc: realloc(ptr, new_size)
             * arg0 = pointer, arg1 = new size
             * Emit: realloc(arg0, arg1) */
            buf_printf(&out, "realloc(%s, %s)", arg_strs[0], arg_strs[1]);
            break;
        case BS_RAW_MEMCPY:
            /* raw-memcpy: memcpy(dest, src, n)
             * arg0 = dest, arg1 = src, arg2 = n
             * Emit as statement: memcpy(arg0, arg1, arg2); */
            indent_buf(body, ctx->indent);
            buf_printf(body, "memcpy(%s, %s, %s);\n", arg_strs[0], arg_strs[1], arg_strs[2]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        case BS_RAW_MEMSET:
            /* raw-memset: memset(dest, byte, n)
             * arg0 = dest, arg1 = byte, arg2 = n
             * Emit as statement: memset(arg0, arg1, arg2); */
            indent_buf(body, ctx->indent);
            buf_printf(body, "memset(%s, %s, %s);\n", arg_strs[0], arg_strs[1], arg_strs[2]);
            buf_free(&out);
            for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
            free(arg_strs);
            return atom_nil();
        /* Phase U3: FFI */
        case BS_DLOPEN:
            /* dlopen: dlopen(path, RTLD_LAZY)
             * arg0 = path (cstr)
             * Emit: dlopen(arg0, RTLD_LAZY) */
            buf_printf(&out, "dlopen(%s, RTLD_LAZY)", arg_strs[0]);
            break;
        case BS_DLSYM:
            /* dlsym: dlsym(handle, symbol)
             * arg0 = handle (ptr<void>), arg1 = symbol (cstr)
             * Emit: dlsym(arg0, arg1) */
            buf_printf(&out, "dlsym(%s, %s)", arg_strs[0], arg_strs[1]);
            break;
        case BS_DLCLOSE:
            /* dlclose: dlclose(handle)
             * arg0 = handle (ptr<void>)
             * Emit: dlclose(arg0) */
            buf_printf(&out, "dlclose(%s)", arg_strs[0]);
            break;
        case BS_FUNC_CALL: {
            /* Generic function call: c_op(arg0, arg1, ...).  The `cons` builtin
             * accepts any 64-bit-sized head/tail (int, cstr, opaque, pointer)
             * -- cast through intptr_t so non-int args don't trip C's
             * "incompatible pointer/int" warning.  Keyed on c_op identity to
             * match the elab-side wildcard in elab_call.c. */
            bool cast_args = (spec->c_op && strcmp(spec->c_op, "cons") == 0);
            buf_printf(&out, "%s(", spec->c_op);
            for (uint32_t i = 0; i < n; i++) {
                if (i > 0) buf_puts(&out, ", ");
                if (cast_args) {
                    buf_printf(&out, "(int64_t)(intptr_t)(%s)", arg_strs[i]);
                } else {
                    buf_puts(&out, arg_strs[i]);
                }
            }
            buf_putc(&out, ')');
            break;
        }
        default:
            /* unreachable for the shapes handled above */
            buf_puts(&out, "((void)0)");
            break;
    }
    buf_putc(&out, '\0');
    char *result = strdup(out.data);
    buf_free(&out);
    for (uint32_t i = 0; i < n; i++) free(arg_strs[i]);
    free(arg_strs);
    return result;
}


/* ------------ entry points ------------ */

/* Phase H §1: Compute the C name of a typeclass instance's dictionary singleton.
 * Mirrors the type_suffix logic in EX_INSTANCE_DEF (emit_stmt) so that the name
 * is consistent wherever it needs to be referenced (EX_DICT emit_value, etc.).
 * Writes "dict_<TypeClass>_<typeargs>" into buf (size buflen). */
void emit_dict_name(char *buf, size_t buflen, const TypeClassInstance *inst) {
    const TypeClass *tc = inst->typeclass;
    char type_suffix[320] = "";  /* wide enough for the longest mangled component (<=259) */
    for (uint8_t i = 0; i < inst->n_type_args; i++) {
        if (i == 0) strncat(type_suffix, "_", sizeof(type_suffix) - strlen(type_suffix) - 1);
        const char *component = "T";
        switch (inst->type_args[i].kind) {
            case TY_INT:      component = "int";      break;
            case TY_BOOL:     component = "bool";     break;
            case TY_CSTR:     component = "cstr";     break;
            case TY_NIL:      component = "nil";      break;
            case TY_PTR_VOID: component = "ptr_void"; break;
            case TY_INT8:     component = "int8";     break;
            case TY_INT16:    component = "int16";    break;
            case TY_INT32:    component = "int32";    break;
            case TY_UINT8:    component = "uint8";    break;
            case TY_UINT16:   component = "uint16";   break;
            case TY_UINT32:   component = "uint32";   break;
            case TY_UINT64:   component = "uint64";   break;
            case TY_FLOAT:    component = "float";    break;
            case TY_FLOAT32:  component = "float32";  break;
            case TY_FLOAT64:  component = "float64";  break;
            case TY_TYVAR:
                /* structdef-retirement slice 5 B2 (P3): unresolved instance head
                 * (unknown name) is a named TY_TYVAR carrying its source symbol;
                 * name the dict by it, mirroring TY_STRUCT / emit_stmt.c so the
                 * elab-side and emit-side dict names stay in lockstep and two
                 * distinct unknown-name instances don't collide on dict_<C>_T. */
                if (inst->type_arg_syms && inst->type_arg_syms[i])
                    component = inst->type_arg_syms[i]->name;
                else if (inst->type_args[i].as.tyvar_.name)
                    component = inst->type_args[i].as.tyvar_.name;
                break;
            case TY_ADT:
                /* CONV-S1 (defstruct-as-defadt): a record-ADT instance head names
                 * its dict by the constructor, exactly as TY_STRUCT -- otherwise
                 * every non-parametric ADT-headed instance collapses to dict_<C>_T
                 * and the emitted dict struct/singleton collide (ODR redefinition).
                 * Mirrors build_inst_type_suffix's TY_ADT arm so the elab-side and
                 * emit-side dict names stay in lockstep. */
                if (inst->type_arg_syms && inst->type_arg_syms[i])
                    component = inst->type_arg_syms[i]->name;
                else if (inst->type_args[i].as.adt_.def &&
                         inst->type_args[i].as.adt_.def->name)
                    component = inst->type_args[i].as.adt_.def->name;
                break;
            case TY_APP: {
                const char *fn_part  = "T";
                const char *arg_part = "T";
                /* MB4 (constrained-hkt-forall-mode-b-plan): keep this in lockstep
                 * with emit_stmt.c's instance-def naming, which prefers the source
                 * symbol (`type_arg_syms`) for a partially-applied instance head
                 * like `(Const r)`.  Reading only the TY_REC name here left the
                 * pass-site dict as `dict_Functor_T_...` while the definition
                 * emitted `dict_Functor_Const_...` -- an undeclared-symbol link
                 * error when an MB1 dict for a parametric functor is passed. */
                if (inst->type_arg_syms && inst->type_arg_syms[i]) {
                    fn_part = inst->type_arg_syms[i]->name;
                } else if (inst->type_args[i].as.app.fn) {
                    Type *fn = inst->type_args[i].as.app.fn;
                    if (fn->kind == TY_REC && fn->as.rec.name)
                        fn_part = fn->as.rec.name;
                }
                if (inst->type_args[i].as.app.arg) {
                    const char *n = type_name(*inst->type_args[i].as.app.arg);
                    if (n) arg_part = n;
                }
                char mfn[128], marg[128], app_comp[260];
                tur_mangle_ident(fn_part, mfn, sizeof(mfn));
                tur_mangle_ident(arg_part, marg, sizeof(marg));
                snprintf(app_comp, sizeof(app_comp), "%s_%s", mfn, marg);
                strncat(type_suffix, app_comp,
                        sizeof(type_suffix) - strlen(type_suffix) - 1);
                continue;
            }
            default: break;
        }
        char comp_buf[128];
        tur_mangle_ident(component, comp_buf, sizeof(comp_buf));
        strncat(type_suffix, comp_buf, sizeof(type_suffix) - strlen(type_suffix) - 1);
    }
    snprintf(buf, buflen, "dict_%s%s", tc->name->name, type_suffix);
}

/* Phase 19D: Collect bindings introduced within an expression (to identify outer captures).
 * defs/ndefs/cdefs is a growable array of Binding*. */
void collect_defined(const Expr *e, Binding ***defs, uint32_t *ndefs, uint32_t *cdefs) {
    if (!e) return;
    switch (e->kind) {
        case EX_LET: {
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                Binding *b = e->as.let_.bindings[i].binding;
                if (b) {
                    if (*ndefs >= *cdefs) {
                        *cdefs = (*cdefs == 0) ? 8 : *cdefs * 2;
                        *defs = (Binding **)realloc(*defs, *cdefs * sizeof(Binding *));
                    }
                    (*defs)[(*ndefs)++] = b;
                }
                collect_defined(e->as.let_.bindings[i].init, defs, ndefs, cdefs);
            }
            collect_defined(e->as.let_.body, defs, ndefs, cdefs);
            break;
        }
        case EX_FN_DEF: {
            if (e->as.fn_def_.fn && e->as.fn_def_.fn->binding) {
                if (*ndefs >= *cdefs) {
                    *cdefs = (*cdefs == 0) ? 8 : *cdefs * 2;
                    *defs = (Binding **)realloc(*defs, *cdefs * sizeof(Binding *));
                }
                (*defs)[(*ndefs)++] = e->as.fn_def_.fn->binding;
            }
            break;
        }
        case EX_FN: {
            if (e->as.fn_.fn && e->as.fn_.fn->binding) {
                if (*ndefs >= *cdefs) {
                    *cdefs = (*cdefs == 0) ? 8 : *cdefs * 2;
                    *defs = (Binding **)realloc(*defs, *cdefs * sizeof(Binding *));
                }
                (*defs)[(*ndefs)++] = e->as.fn_.fn->binding;
            }
            break;
        }
        case EX_DO: {
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                collect_defined(e->as.do_.items[i], defs, ndefs, cdefs);
            break;
        }
        case EX_IF: {
            collect_defined(e->as.if_.cond, defs, ndefs, cdefs);
            collect_defined(e->as.if_.then_, defs, ndefs, cdefs);
            collect_defined(e->as.if_.else_or_null, defs, ndefs, cdefs);
            break;
        }
        case EX_WHILE: {
            collect_defined(e->as.while_.cond, defs, ndefs, cdefs);
            collect_defined(e->as.while_.body, defs, ndefs, cdefs);
            break;
        }
        case EX_HANDLE: {
            /* Walk the handle body to find bindings defined within it */
            if (e->as.handle_.handle)
                collect_defined(e->as.handle_.handle->body, defs, ndefs, cdefs);
            break;
        }
        default:
            break;
    }
}

/* Append a binding to a caps array (realloc-growing) unless it is a global,
 * a TY_FN value, already present, or in the `defs` (locally-defined) set.
 * Shared by the nested-handle merge below. */
static void cap_append(Binding *b, Binding ***caps, uint32_t *ncaps, uint32_t *ccaps,
                       Binding **defs, uint32_t ndefs) {
    /* Skip globals only. A non-global TY_FN binding is a genuine local
     * fn-value (a colored fn-value parameter or a let-bound closure) and
     * must be captured into the handle body/handler env like any other
     * local -- see collect_handle_captures EX_VAR/EX_CALL. */
    if (!b || b->is_global) return;
    for (uint32_t i = 0; i < ndefs; i++)
        if (defs[i] == b) return;
    for (uint32_t i = 0; i < *ncaps; i++)
        if ((*caps)[i] == b) return;
    if (*ncaps >= *ccaps) {
        *ccaps = (*ccaps == 0) ? 8 : *ccaps * 2;
        *caps = (Binding **)realloc(*caps, *ccaps * sizeof(Binding *));
    }
    (*caps)[(*ncaps)++] = b;
}

/* Phase 19D: Collect free variable bindings in an expression (bindings not defined within it).
 * Returns a malloc'd array of outer bindings; caller frees. *n_out is set to count. */
Binding **collect_handle_captures(const Expr *body, uint32_t *n_out) {
    /* Step 1: collect all bindings defined within body */
    Binding **defs = NULL;
    uint32_t ndefs = 0, cdefs = 0;
    collect_defined(body, &defs, &ndefs, &cdefs);

    /* Step 2: walk body for EX_VAR refs not in defined set and not already captured */
    Binding **caps = NULL;
    uint32_t ncaps = 0, ccaps = 0;

    /* Use a simple recursive walk via a stack */
    /* We'll use a small helper approach: collect all EX_VAR bindings from body */
    /* then filter out those in defs */
    /* Simple DFS via recursion - collect all variable references */
    /* For now, implement a conservative approach: collect all EX_VAR that are not
     * defined within body and not globals */
    typedef struct ExprStack { const Expr *e; struct ExprStack *next; } ExprStack;
    ExprStack *stack = NULL;
    ExprStack initial = { body, NULL };
    stack = &initial;

    /* We need heap-allocated stack nodes for recursive traversal */
    /* Use a simple iterative approach with malloc'd stack */
    ExprStack **heap_nodes = NULL;
    uint32_t heap_count = 0, heap_cap = 0;

#define PUSH_EXPR(ex) do { \
    if (ex) { \
        if (heap_count >= heap_cap) { \
            heap_cap = (heap_cap == 0) ? 16 : heap_cap * 2; \
            heap_nodes = (ExprStack **)realloc(heap_nodes, heap_cap * sizeof(ExprStack *)); \
        } \
        ExprStack *_node = (ExprStack *)malloc(sizeof(ExprStack)); \
        _node->e = (ex); _node->next = stack; stack = _node; \
        heap_nodes[heap_count++] = _node; \
    } \
} while(0)

    /* Reset stack - just use heap nodes from the start */
    stack = NULL;
    PUSH_EXPR(body);

    while (stack) {
        ExprStack *top = stack;
        stack = top->next;
        const Expr *cur = top->e;
        if (!cur) continue;

        switch (cur->kind) {
            case EX_VAR: {
                Binding *b = cur->as.var.binding;
                /* Skip globals (top-level named fns/vars emit as bare C
                 * symbols and need no env slot). A TY_FN binding that is NOT
                 * global is a genuine local fn-value -- a colored fn-value
                 * PARAMETER (e.g. `f : (fn [] #fx{E} int)`) or a let-bound
                 * closure -- referenced from the delimited body. It must be
                 * captured into the fiber body's env like any other local, or
                 * the emitted handle-body fn references it undeclared. */
                if (!b || b->is_global) break;
                /* Check if defined within body */
                bool in_defs = false;
                for (uint32_t i = 0; i < ndefs; i++) {
                    if (defs[i] == b) { in_defs = true; break; }
                }
                if (in_defs) break;
                /* Check if already captured */
                bool already = false;
                for (uint32_t i = 0; i < ncaps; i++) {
                    if (caps[i] == b) { already = true; break; }
                }
                if (!already) {
                    if (ncaps >= ccaps) {
                        ccaps = (ccaps == 0) ? 8 : ccaps * 2;
                        caps = (Binding **)realloc(caps, ccaps * sizeof(Binding *));
                    }
                    caps[ncaps++] = b;
                }
                break;
            }
            case EX_LET: {
                for (uint32_t i = 0; i < cur->as.let_.n; i++)
                    PUSH_EXPR(cur->as.let_.bindings[i].init);
                PUSH_EXPR(cur->as.let_.body);
                break;
            }
            case EX_DO: {
                for (uint32_t i = 0; i < cur->as.do_.n; i++)
                    PUSH_EXPR(cur->as.do_.items[i]);
                break;
            }
            case EX_IF: {
                PUSH_EXPR(cur->as.if_.cond);
                PUSH_EXPR(cur->as.if_.then_);
                PUSH_EXPR(cur->as.if_.else_or_null);
                break;
            }
            case EX_WHILE: {
                PUSH_EXPR(cur->as.while_.cond);
                PUSH_EXPR(cur->as.while_.body);
                break;
            }
            case EX_SET: {
                /* The target binding is also a reference (it's being mutated) */
                if (cur->as.set_.target) {
                    Binding *b = cur->as.set_.target;
                    /* Mirror EX_VAR: a non-global TY_FN target is a local
                     * fn-value being reassigned; it still needs an env slot. */
                    if (!b->is_global) {
                        /* Check if defined within body */
                        bool in_defs = false;
                        for (uint32_t i = 0; i < ndefs; i++) {
                            if (defs[i] == b) { in_defs = true; break; }
                        }
                        if (!in_defs) {
                            bool already = false;
                            for (uint32_t i = 0; i < ncaps; i++) {
                                if (caps[i] == b) { already = true; break; }
                            }
                            if (!already) {
                                if (ncaps >= ccaps) {
                                    ccaps = (ccaps == 0) ? 8 : ccaps * 2;
                                    caps = (Binding **)realloc(caps, ccaps * sizeof(Binding *));
                                }
                                caps[ncaps++] = b;
                            }
                        }
                    }
                }
                PUSH_EXPR(cur->as.set_.value);
                break;
            }
            case EX_CALL: {
                for (uint32_t i = 0; i < cur->as.call_.n_args; i++)
                    PUSH_EXPR(cur->as.call_.args[i]);
                PUSH_EXPR(cur->as.call_.fn_expr);
                /* A direct call through a fn-value binding (e.g. `(f)` where
                 * `f` is a colored fn-value parameter) stores the callee in
                 * `fn_binding`, not `fn_expr`, so it is never seen as an
                 * EX_VAR by this walk. Capture it explicitly: when it is a
                 * non-global local it must be threaded into the handle body's
                 * env, or the emitted fiber body references it undeclared. */
                cap_append(cur->as.call_.fn_binding, &caps, &ncaps, &ccaps,
                           defs, ndefs);
                break;
            }
            case EX_BUILTIN: {
                for (uint32_t i = 0; i < cur->as.builtin.n; i++)
                    PUSH_EXPR(cur->as.builtin.args[i]);
                break;
            }
            case EX_RETURN: {
                PUSH_EXPR(cur->as.return_.value);
                break;
            }
            case EX_PERFORM: {
                if (cur->as.perform_.perform) {
                    for (uint32_t i = 0; i < cur->as.perform_.perform->n_args; i++)
                        PUSH_EXPR(cur->as.perform_.perform->args[i]);
                }
                break;
            }
            case EX_HANDLE: {
                if (cur->as.handle_.handle) {
                    HandleExpr *ih = cur->as.handle_.handle;
                    /* Walk the inner handle body normally. */
                    PUSH_EXPR(ih->body);
                    /* A nested handle emits its own env-fill (`__henv_N->f = f`)
                     * in the CURRENT function, referencing each of the inner
                     * handle's captures by name. Those captures include free
                     * variables of the inner CASE bodies, which are otherwise
                     * skipped (case bodies have their own param/k scope). So we
                     * must thread the inner handle's transitive case-body
                     * captures through this env too -- mirroring EX_CLOSURE.
                     * The inner case's own effect-params and k are local to it
                     * and must not leak outward. */
                    for (uint8_t ci = 0; ci < ih->n_cases; ci++) {
                        HandleCase *icase = &ih->cases[ci];
                        uint32_t n_icaps = 0;
                        Binding **icaps = collect_handle_captures(icase->body, &n_icaps);
                        for (uint32_t j = 0; j < n_icaps; j++) {
                            Binding *ib = icaps[j];
                            bool inner_local = (icase->k_binding && ib == icase->k_binding);
                            for (uint32_t p = 0; p < icase->n_params && !inner_local; p++)
                                if (icase->param_bindings && icase->param_bindings[p] == ib)
                                    inner_local = true;
                            if (!inner_local)
                                cap_append(ib, &caps, &ncaps, &ccaps, defs, ndefs);
                        }
                        free(icaps);
                    }
                }
                break;
            }
            case EX_RESUME: {
                if (cur->as.resume_.resume) {
                    PUSH_EXPR(cur->as.resume_.resume->k);
                    PUSH_EXPR(cur->as.resume_.resume->value);
                }
                break;
            }
            case EX_DISCONTINUE: {
                if (cur->as.discontinue_.discontinue) {
                    PUSH_EXPR(cur->as.discontinue_.discontinue->k);
                    PUSH_EXPR(cur->as.discontinue_.discontinue->exception);
                }
                break;
            }
            case EX_CONT_PRED: {
                PUSH_EXPR(cur->as.cont_pred_.expr);
                break;
            }
            case EX_CALLCC: {
                /* (call/cc f) / (escape f): the receiver `f` (a closure at emit
                 * time) captures enclosing locals through its env, and its
                 * env-init (`__t->field = <name>`) references those names in the
                 * CURRENT function -- so they must be threaded into the handler
                 * body's env just like any other free variable.  Push the
                 * receiver so the EX_CLOSURE case folds its pre-computed captures
                 * (mirrors EX_FN_TO_FAT / EX_CLOSURE).  Without this a capturing
                 * escape in a handler case references an undeclared C name. */
                PUSH_EXPR(cur->as.callcc_.fn);
                break;
            }
            case EX_FN_TO_FAT: {
                /* A#1 auto-shim wrapper: descend to the inner fn so its closure
                 * captures are collected (KB-IDIOM-1). */
                PUSH_EXPR(cur->as.fn_to_fat_.inner);
                break;
            }
            case EX_REF: { PUSH_EXPR(cur->as.ref_.expr); break; }
            case EX_DEREF: { PUSH_EXPR(cur->as.deref_.expr); break; }
            /* Owning-value ops (rc/of, rc/clone, rc/drop, rc->ptr,
             * rc/strong-count, rc/from-ref, ref/from-rc, weak, upgrade, weak?,
             * ref?): each wraps a single operand `.expr` whose free variables
             * must be captured. A handler case that references a local only
             * through such an op -- e.g. `(rc/drop (.r o))` -- would otherwise
             * leave `o` uncaptured and emit an undeclared C name in the
             * handler body. All eleven share the `{ Expr *expr; ... }` leading
             * layout, so `rc_of_.expr` reads the operand for every one. */
            case EX_RC_OF:
            case EX_RC_CLONE:
            case EX_RC_DROP:
            case EX_RC_PTR:
            case EX_RC_COUNT:
            case EX_RC_FROM_REF:
            case EX_REF_FROM_RC:
            case EX_WEAK:
            case EX_WEAK_UPGRADE:
            case EX_WEAK_PRED:
            case EX_REF_PRED: {
                PUSH_EXPR(cur->as.rc_of_.expr);
                break;
            }
            case EX_GET_FIELD: {
                PUSH_EXPR(cur->as.get_field_.struct_expr);
                break;
            }
            case EX_SET_FIELD: {
                PUSH_EXPR(cur->as.set_field_.value);
                PUSH_EXPR(cur->as.set_field_.receiver);
                break;
            }
            case EX_BORROW_IMMUT: {
                PUSH_EXPR(cur->as.borrow_immut_.expr);
                break;
            }
            case EX_BORROW_MUT: {
                PUSH_EXPR(cur->as.borrow_mut_.expr);
                break;
            }
            case EX_DEFER: {
                /* Defer body's captures are pre-computed; add them directly.
                 * Walking the defer body would require separate collect_defined
                 * tracking for bindings defined inside the defer. */
                for (uint8_t j = 0; j < cur->as.defer_.n_captures; j++) {
                    Binding *db = cur->as.defer_.captures[j];
                    if (!db || db->is_global || db->type.kind == TY_FN) continue;
                    bool in_defs = false;
                    for (uint32_t k = 0; k < ndefs; k++) {
                        if (defs[k] == db) { in_defs = true; break; }
                    }
                    if (in_defs) continue;
                    bool already = false;
                    for (uint32_t k = 0; k < ncaps; k++) {
                        if (caps[k] == db) { already = true; break; }
                    }
                    if (!already) {
                        if (ncaps >= ccaps) {
                            ccaps = (ccaps == 0) ? 8 : ccaps * 2;
                            caps = (Binding **)realloc(caps, ccaps * sizeof(Binding *));
                        }
                        caps[ncaps++] = db;
                    }
                }
                break;
            }
            case EX_CLOSURE: {
                /* KB-IDIOM-1: a closure constructed inside the handle body
                 * captures free variables from the enclosing scope. Its env-init
                 * (__t->field = <name>) references those names directly, so they
                 * must be threaded into the handle body's env (body_env) just like
                 * any other free variable. Do NOT walk the fn body (it has its own
                 * param/local scope); instead add the closure's pre-computed
                 * captures, which are already transitive (an outer closure captures
                 * everything its inner closures need from further out). */
                struct Closure *cl = cur->as.closure_.closure;
                if (cl) {
                    for (uint8_t j = 0; j < cl->n_captures; j++) {
                        Binding *cb = cl->captures[j];
                        if (!cb || cb->is_global || cb->type.kind == TY_FN) continue;
                        bool in_defs = false;
                        for (uint32_t k = 0; k < ndefs; k++) {
                            if (defs[k] == cb) { in_defs = true; break; }
                        }
                        if (in_defs) continue;
                        bool already = false;
                        for (uint32_t k = 0; k < ncaps; k++) {
                            if (caps[k] == cb) { already = true; break; }
                        }
                        if (!already) {
                            if (ncaps >= ccaps) {
                                ccaps = (ccaps == 0) ? 8 : ccaps * 2;
                                caps = (Binding **)realloc(caps, ccaps * sizeof(Binding *));
                            }
                            caps[ncaps++] = cb;
                        }
                    }
                }
                break;
            }
            case EX_ASCRIBE: {
                /* (:: expr type) is erased at codegen but its inner expression
                 * still references variables that must be captured. Descend so a
                 * variable used only inside an ascription is collected. */
                PUSH_EXPR(cur->as.ascribe_.inner);
                break;
            }
            case EX_REINTERPRET: {
                /* Bit-reinterpret wrapper: descend into the operand so its var
                 * references are captured like a bare reference would be. */
                PUSH_EXPR(cur->as.reinterpret_.expr);
                break;
            }
            default:
                break;
        }
    }
#undef PUSH_EXPR

    /* Free heap nodes */
    for (uint32_t i = 0; i < heap_count; i++) free(heap_nodes[i]);
    free(heap_nodes);
    free(defs);

    *n_out = ncaps;
    return caps;
}

/* ------------ Phase ACB: emit_carrier_bridge ------------ */

/* Returns true when concrete_ty fits entirely in an int64_t-wide slot and
 * the carrier stores the value inline (bitwise reinterpret) rather than as
 * a heap pointer.  Scalars with type_size_bytes == 8 qualify; pointer-sized
 * types (TY_CSTR, TY_PTR_VOID) are already int64_t-compatible so no
 * reinterpret is needed and this returns false for them.
 * Struct/ADT/composite types have size 0 in type_size_bytes and are always
 * pointer carriers. */
static bool carrier_is_inline(TypeKind k) {
    switch (k) {
        case TY_FLOAT:
        case TY_FLOAT64:
        case TY_FLOAT32:
        case TY_INT32:
        case TY_UINT32:
        case TY_INT16:
        case TY_UINT16:
        case TY_INT8:
        case TY_UINT8:
        case TY_BOOL:
            return true;
        default:
            return false;
    }
}

/* inline-c-builders-cannot-nest-option-in-result: is `src` the temp of a
 * fresh inline-C sum box that RM1 frees after its consuming call (the
 * sum_pending list)?  Such a box has the builders' layout. */
static bool emit_src_is_pending_sum_box(const EmitCtx *ctx, const char *src) {
    if (!src || !emit_str_is_bare_ident(src)) return false;
    for (uint32_t pi = 0; pi < ctx->n_sum_pending; pi++)
        if (ctx->sum_pending[pi] && strcmp(ctx->sum_pending[pi], src) == 0)
            return true;
    return false;
}

/* inline-c-builders-cannot-nest-option-in-result: read an inline-C body's
 * OWNED carrier box back into a by-value sum monomorph whose payload is
 * itself a sum -- `(Result (Option cstr) E)` built as
 * `tur_ok_int(tur_some_ptr(s))`.  The builders make a 16-byte
 * `{ tag; word }` box whose word is the INNER builder box, while the
 * monomorph stores its Option payload BY VALUE (24 bytes), so the plain
 * `*(T *)box` readback read the inner tag out of a pointer's low bytes:
 * neither arm of a nested match fired, and nothing said so.
 *
 * Here the outer tag is copied, and each payload word whose field type is a
 * non-heap sum goes through the carrier->concrete bridge as an owned box of
 * its own (which recurses for deeper nesting, unwraps a niche Option, and
 * frees the inner box); any other payload is the same bytes the plain
 * readback copied.  Returns false -- emitting nothing -- when no field nests
 * a sum, or a constructor has more than one field (no builder makes one), so
 * every other shape keeps the plain deref.  A null carrier is the zeroed
 * aggregate, which is None for an Option-shaped outer. */
static bool emit_nested_sum_box_applies(EmitCtx *ctx, Type concrete_ty) {
    Type rty = emit_resolve_type(ctx, concrete_ty);
    AdtDef *adt = NULL;
    Type args[16];
    uint8_t nargs = 0;
    if (!type_extract_adt_app(&rty, &adt, args, &nargs) || !adt ||
        adt_uses_named_layout(adt) || adt->is_heap || adt->n_ctors < 2)
        return false;
    bool any_nested = false;
    for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
        const CtorDef *c = adt->ctors[ci];
        if (!c) return false;
        if (c->n_fields > 1) return false;
        if (c->n_fields == 0 || !c->fields[0].full_type) continue;
        Type ft = substitute_adt_app_type_owned(c->fields[0].full_type, adt, args);
        Type rft = emit_resolve_type(ctx, ft);
        AdtDef *fd = NULL;
        Type fargs[16];
        uint8_t nf = 0;
        if (type_extract_adt_app(&rft, &fd, fargs, &nf) && fd && !fd->is_heap &&
            !fd->is_opaque && fd->n_ctors >= 2)
            any_nested = true;
        free_struct_app_type(ft);
    }
    return any_nested;
}

static bool emit_readback_nested_sum_box(EmitCtx *ctx, Buf *body,
                                         const char *cname, Type concrete_ty,
                                         const char *ctmp, const char *vtmp) {
    if (!emit_nested_sum_box_applies(ctx, concrete_ty)) return false;
    Type rty = emit_resolve_type(ctx, concrete_ty);
    AdtDef *adt = NULL;
    Type args[16];
    uint8_t nargs = 0;
    type_extract_adt_app(&rty, &adt, args, &nargs);

    indent_buf(body, ctx->indent);
    buf_printf(body, "%s %s;\n", cname, vtmp);
    indent_buf(body, ctx->indent);
    buf_printf(body, "memset(&%s, 0, sizeof %s);\n", vtmp, vtmp);
    indent_buf(body, ctx->indent);
    buf_printf(body, "if (%s) {\n", ctmp);
    ctx->indent += 4;
    char *nb = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "tur_result_box_t *%s = (tur_result_box_t *)(intptr_t)(%s);\n",
               nb, ctmp);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s.tag = %s->tag;\n", vtmp, nb);
    indent_buf(body, ctx->indent);
    buf_printf(body, "switch (%s->tag) {\n", nb);
    for (uint32_t ci = 0; ci < adt->n_ctors; ci++) {
        const CtorDef *c = adt->ctors[ci];
        if (c->n_fields == 0) continue;
        char *path = adt_field_member_path(adt, c, 0);
        indent_buf(body, ctx->indent);
        buf_printf(body, "case %u: {\n", (unsigned)c->tag);
        ctx->indent += 4;
        Type ft = substitute_adt_app_type_owned(c->fields[0].full_type, adt, args);
        Type rft = emit_resolve_type(ctx, ft);
        AdtDef *fd = NULL;
        Type fargs[16];
        uint8_t nf = 0;
        bool nested = type_extract_adt_app(&rft, &fd, fargs, &nf) && fd &&
                      !fd->is_heap && !fd->is_opaque && fd->n_ctors >= 2;
        if (nested) {
            /* The word is the inner builder's box, owned with the outer. */
            char *w = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "int64_t %s = %s->as.ok_val;\n", w, nb);
            emit_localvar_record_ctype(w, "int64_t");
            emit_owned_carrier_mark(w);
            char *iv = emit_carrier_bridge(ctx, body, strdup(w), CK_CARRIER,
                                           CK_CONCRETE, rft);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s.%s = %s;\n", vtmp, path, iv);
            emit_owned_carrier_clear(w);
            free(iv);
            free(w);
        } else {
            indent_buf(body, ctx->indent);
            buf_printf(body, "memcpy(&%s.%s, &%s->as.ok_val, sizeof %s.%s);\n",
                       vtmp, path, nb, vtmp, path);
        }
        free_struct_app_type(ft);
        indent_buf(body, ctx->indent);
        buf_puts(body, "break;\n");
        ctx->indent -= 4;
        indent_buf(body, ctx->indent);
        buf_puts(body, "}\n");
        free(path);
    }
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");
    free(nb);
    ctx->indent -= 4;
    indent_buf(body, ctx->indent);
    buf_puts(body, "}\n");
    return true;
}

char *emit_carrier_bridge(EmitCtx *ctx, Buf *body,
                          char *src_str,
                          CarrierKind src_ck, CarrierKind sink_ck,
                          Type concrete_ty) {
    /* No crossing needed. */
    if (src_ck == sink_ck) return src_str;

    /* SC7: a transparent int newtype (including the lowered single-int-field
     * record-ADT form) has the SAME C representation -- a bare int64 -- in both
     * the carrier and the concrete world, so the crossing is the identity in
     * either direction.  Return the source verbatim before the aggregate-spill
     * path below would take its address (`(int64_t)(intptr_t)(&tmp)`) and hand a
     * stack pointer to an int64-by-value formal. */
    if (type_is_transparent_int_newtype(concrete_ty)) return src_str;

    /* VBM2b (van-laarhoven-monomorphization): inside a monomorphized spec body a
     * by-value `(f a)` receiver may still be spelled with its generic tyvar arg
     * (`(Identity A)`), which `emit_type_c_name` collapses to the int64 carrier
     * -- so the spill temp comes out `int64_t __t = i` and mistypes a real
     * aggregate.  When the active spec resolves that tyvar-headed app to a
     * concrete by-value ADT app, spill at the concrete type instead.  No-op
     * outside a spec, and for an already-concrete or still-unbound app (the
     * resolve is idempotent / leaves it non-concrete). */
    if (ctx && ctx->current_abi_specialization && concrete_ty.kind == TY_APP &&
        !type_app_is_concrete_adt(&concrete_ty)) {
        Type resolved = emit_resolve_type(ctx, concrete_ty);
        if (resolved.kind == TY_APP && type_app_is_concrete_adt(&resolved))
            concrete_ty = resolved;
    }

    /* M3 audit: identify which call site reaches the bridge; trace tagged so
     * the per-fixture cost of removing it is measurable.  Enable with
     * TUR_M3_AUDIT=1. */
    if (getenv("TUR_M3_AUDIT")) {
        const char *dir = (src_ck == CK_CARRIER && sink_ck == CK_CONCRETE)
            ? "carrier->concrete" : "concrete->carrier";
        fprintf(stderr, "[m3-audit] bridge %s type=%s\n",
                dir, type_name(concrete_ty));

        /* Phase 5.1 tripwire: assert the crossing stays inside the
         * carrier-essential family.  The 2026-06-19 re-audit
         * (v2/m7-phase5-carrier-bridge-audit.md) established that every real
         * crossing is one of: a :heap-tagged handle (Vec/Map/Set/...), an
         * Option/Result by-value struct meeting a dict/indirect-dispatch
         * carrier producer, or an inline 8-byte scalar.  The legacy
         * field-by-field-or-deref fallback below also accepts an arbitrary
         * non-Option/Result struct (Pair, user defstruct), but the audit floor
         * for that set is ZERO.  Surfacing such a crossing here catches an ABI
         * regression at compile-audit time -- e.g. a new generic instance body
         * that leaks a by-value aggregate through the carrier -- instead of at
         * the next manual sweep.  A hard abort waits on the dict-ABI
         * monomorphization (5.3/5.5); under the audit env-var this stays a
         * non-fatal tripwire so the gate suite is never disturbed. */
        bool essential = type_is_heap_struct(concrete_ty) ||
                         carrier_is_inline(concrete_ty.kind);
        if (!essential) {
            /* structdef-retirement DS-D: the former struct-app Option/Result
             * check (type_extract_struct_app) is dead -- no struct-headed app
             * forms; a lowered Option/Result monomorph is an ADT app. */
            /* Pointer-sized leaves (cstr, ptr<void>, int/int64) cross with no
             * reinterpret and are trivially safe -- not part of the
             * struct-fallback set the tripwire watches. */
            switch (concrete_ty.kind) {
                case TY_CSTR: case TY_PTR_VOID:
                case TY_INT: case TY_INT64: case TY_UINT64:
                    essential = true;
                    break;
                default: break;
            }
        }
        if (!essential) {
            fprintf(stderr,
                "[m3-audit] WARNING non-essential carrier crossing type=%s "
                "(expected floor is Option/Result/heap/inline-scalar only; see "
                "docs/archive/m7-phase5-carrier-bridge-audit.md)\n",
                type_name(concrete_ty));
        }
    }

    /* repr-trace (representation-consolidation-meta-plan increment 0):
     * with --emit-abi-trace, print every carrier<->concrete crossing this
     * chokepoint lowers, with the lowering form it picked -- the
     * value-position counterpart of the fn-param decision lines in
     * elab_fns.c.  (TUR_M3_AUDIT above is the older, env-gated variant
     * with its own tripwire; both stay.) */
    if (g_emit_abi_trace) {
        const char *dir = (src_ck == CK_CARRIER && sink_ck == CK_CONCRETE)
            ? "carrier->concrete" : "concrete->carrier";
        const char *form =
            (type_is_heap_struct(concrete_ty) || type_is_heap_adt(concrete_ty))
                ? "heap-reinterpret"
                : carrier_is_inline(concrete_ty.kind) ? "inline-reinterpret"
                                                      : "aggregate";
        fprintf(stderr, "repr-trace bridge %s %s %s\n",
                dir, form, type_name(concrete_ty));

        /* Increment 4 stage 3: the PER-ARG BRIDGE shadow.  This chokepoint is
         * the whole position -- every per-argument representation crossing in
         * emit_expr.c routes through here (the escaping sibling delegates),
         * so one check covers what the plan called "the long tail" without
         * touching the ~24 call sites individually.
         *
         * The invariant is the one the method-result shadow settled on: a
         * crossing is a contract that `concrete_ty` really is the CONCRETE
         * side.  If the protocol calls that type the erased carrier, the
         * bridge is about to spill, address, or reinterpret an int64 as
         * though it were a distinct representation -- crossing a boundary
         * that is not there.  Position PARAM, because a per-arg crossing
         * lands in a parameter slot and that is where a type's concrete
         * spelling is decided. */
    }
    if (repr_shadow_active() &&
        repr_of(&concrete_ty, REPR_POS_PARAM) == REPR_CARRIER_I64) {
        const char *dir2 = (src_ck == CK_CARRIER && sink_ck == CK_CONCRETE)
            ? "carrier->concrete" : "concrete->carrier";
        char line[512];
        snprintf(line, sizeof line,
                 "repr-shadow arg-bridge param type=%s want=concrete "
                 "got=carrier-i64 dir=%s\n", type_name(concrete_ty), dir2);
        repr_shadow_disagree("arg-bridge", false, line);
    }

    const char *cname = emit_type_c_name(ctx, concrete_ty);
    Buf out;
    buf_init(&out);

    /* SR3 slice B: the niche crossing, and it is a real VALUE change -- not a
     * spill, not a reinterpret.  A niche `(Option P)` IS the payload pointer; a
     * CARRIER `(Option A)` is a pointer to a tagged box.  Both spell None as
     * NULL (slice A), so only Some actually moves: box it on the way out,
     * unbox it on the way back.
     *
     * Without this the concrete->carrier arm below spills the niche pointer to
     * a stack temp and hands its ADDRESS to a base that reads `->tag`, so the
     * base reads the low half of the payload pointer as the tag, matches
     * neither 0 nor 1, and falls out of the switch with the result temp at its
     * zero init.  That is a SILENT wrong answer -- `option-of-tvec-eq` printed
     * `false` for two equal `(some v)` -- which is the failure class this whole
     * family ships and the reason the gate asserts values, not builds. */
    if (adt_app_is_niche_option(concrete_ty)) {
        /* Inline-C carrier producer feeding a carrier sink: the "concrete"
         * source is a bare temp whose RECORDED emitted spelling is already the
         * int64 carrier word (`(vec-push! v (mk-c 1))` -- mk-c's inline-C body
         * returns tur_some_ptr's box).  Its TYPE is the niche, which is what
         * routed the call here, but the VALUE never took the niche form, so
         * "materialize the carrier from the niche" would wrap the box in a
         * second box (`tur_box_some(box)`) and the reader would unwrap one
         * layer and hand the inner box to the consumer as the payload.  The
         * value is already exactly what the carrier sink wants: pass it
         * through.  Same key as every other inline-C-producer bridge (the
         * localvar side table), so a genuine niche value -- whose recorded
         * spelling is the payload's pointer type -- still materializes. */
        /* container-element-form-plan CE2: a Vec slot holds a niche element
         * as its payload WORD (container_elem_form == CE_WORD), not as a
         * carrier box.  The store half: the argument loop flags a Vec element
         * store sink (ctx->ce_word_store_sink) and the value goes into the
         * slot as the pointer it already is.  An inline-C producer's box
         * (recorded int64 spelling -- `(vec-push! v (mk-c 1))`) is unboxed to
         * its payload first and released: the box was the caller's by the
         * inline-C contract, and nothing else ever holds it.  The read half:
         * a temp the hoist marked as a raw slot word (vec-get / vec-pop! /
         * vec-data-get-checked__) IS the niche form and is cast, never
         * unwrapped.  Both halves are one convention; every other position
         * keeps the boxing crossing below, so a niche value handed to an
         * erased READER still meets the tagged layout that reader expects. */
        if (src_ck == CK_CONCRETE && sink_ck == CK_CARRIER &&
            ctx->ce_word_store_sink &&
            container_elem_form(concrete_ty) == CE_WORD) {
            const char *rec = emit_str_is_bare_ident(src_str)
                ? emit_localvar_lookup_ctype(src_str) : NULL;
            char *wtmp = fresh_tmp(ctx);
            if (rec && strcmp(rec, "int64_t") == 0) {
                char *btmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body, "int64_t %s = (int64_t)(intptr_t)(%s);\n",
                           btmp, src_str);
                indent_buf(body, ctx->indent);
                buf_printf(body, "int64_t %s = (%s ? tur_opt_value_checked(%s) : (int64_t)0);\n",
                           wtmp, btmp, btmp);
                indent_buf(body, ctx->indent);
                buf_printf(body, "if (%s) free((void *)(intptr_t)(%s));\n", btmp, btmp);
                emit_owned_carrier_clear(src_str);
                free(btmp);
            } else {
                indent_buf(body, ctx->indent);
                buf_printf(body, "int64_t %s = (int64_t)(intptr_t)(%s);\n",
                           wtmp, src_str);
            }
            free(src_str);
            buf_free(&out);
            return wtmp;
        }
        if (src_ck == CK_CARRIER && sink_ck == CK_CONCRETE &&
            emit_str_is_bare_ident(src_str) && emit_slot_word_is(src_str) &&
            container_elem_form(concrete_ty) == CE_WORD) {
            buf_printf(&out, "((%s)(intptr_t)(%s))", cname, src_str);
            free(src_str);
            char *wres = strdup(out.data);
            buf_free(&out);
            return wres;
        }
        if (src_ck == CK_CONCRETE && sink_ck == CK_CARRIER &&
            emit_str_is_bare_ident(src_str)) {
            const char *rec = emit_localvar_lookup_ctype(src_str);
            if (rec && strcmp(rec, "int64_t") == 0) {
                buf_free(&out);
                return src_str;
            }
        }
        /* Asked BEFORE src_str is freed below, and only for the unboxing
         * direction: the carrier box the mark names is the thing this crossing
         * consumes.  See the owning branch for what the mark means. */
        bool owns_niche_box = src_ck == CK_CARRIER && sink_ck == CK_CONCRETE &&
                              emit_str_is_bare_ident(src_str) &&
                              emit_owned_carrier_is(src_str);
        char *ntmp = fresh_tmp(ctx);
        indent_buf(body, ctx->indent);
        if (src_ck == CK_CONCRETE && sink_ck == CK_CARRIER) {
            buf_printf(body, "%s %s = (%s);\n", cname, ntmp, src_str);
            buf_printf(&out,
                       "(%s ? tur_box_some((int64_t)(intptr_t)(%s)) : (int64_t)0)",
                       ntmp, ntmp);
        } else {
            buf_printf(body, "int64_t %s = (int64_t)(intptr_t)(%s);\n",
                       ntmp, src_str);
            /* The CHECKED read: a carrier box can hold Some(NULL)
             * (tur_some_ptr(0)), and the unchecked tur_opt_value would hand
             * the niche its 0 payload -- silently turning a value `some?`
             * calls true into `(none)`.  Same declaration the niche Some ctor
             * enforces, at the other door. */
            if (owns_niche_box) {
                /* inline-c-option-carrier-box-leaks, the NICHE half.  This arm
                 * had the consumption its two siblings already carry -- the
                 * by-value readback below, and the CE_WORD Vec-slot store just
                 * above -- and it is the arm an inline-C `: (Option String)`
                 * producer actually reaches, because a niche Option is exactly
                 * an Option whose payload is a pointer.
                 *
                 * `tur_opt_value_checked` COPIES the payload word out of the
                 * box, so once the copy is materialized the box is dead; the
                 * expression form in the else branch has nowhere to put the
                 * free, which is why it leaked 16 bytes per call.  Read into a
                 * temp first, THEN free -- the read is a load out of the
                 * allocation being freed and must not be reordered after it.
                 *
                 * The null test is the same one the ternary already needed: a
                 * niche None is the null carrier (SR3 slice A), allocates
                 * nothing, and must not be freed. */
                char *vtmp = fresh_tmp(ctx);
                indent_buf(body, ctx->indent);
                buf_printf(body,
                           "%s %s = (%s ? (%s)(intptr_t)tur_opt_value_checked(%s) : (%s)0);\n",
                           cname, vtmp, ntmp, cname, ntmp, cname);
                indent_buf(body, ctx->indent);
                buf_printf(body, "if (%s) free((void *)(intptr_t)(%s));\n",
                           ntmp, ntmp);
                buf_printf(&out, "%s", vtmp);
                /* Consume the mark: a temp bridged twice must not be freed
                 * twice -- the same discipline as the by-value readback. */
                emit_owned_carrier_clear(src_str);
                free(vtmp);
            } else {
                buf_printf(&out, "(%s ? (%s)(intptr_t)tur_opt_value_checked(%s) : (%s)0)",
                           ntmp, cname, ntmp, cname);
            }
        }
        free(ntmp);
        free(src_str);
        char *nres = strdup(out.data);
        buf_free(&out);
        return nres;
    }

    /* end-to-end-monomorphization (C-3): a :heap-tagged type (Vec/Map/Set/...)
     * is represented as a typed pointer `T__A *` whose bit pattern IS the int64
     * carrier.  Crossing the carrier boundary is therefore a pure reinterpret
     * cast, never a struct deref-copy (CK_CARRIER->CONCRETE) or address-of
     * spill (CK_CONCRETE->CARRIER): the value is already pointer-sized and
     * shared by identity.  Without this, a heap receiver flowing through the
     * carrier (e.g. the abstract `vec-new` base result feeding a `(Vec A)`
     * spec param) emitted `*(int64_t *)(intptr_t)(handle)` -- a wild deref of
     * the header's first word, segfaulting at runtime. */
    /* opaque-pointer-c-spelling: an opaque newtype over a pointer is the
     * same shape as the :heap case just described -- a one-word handle whose
     * bit pattern IS the int64 carrier -- so its carrier crossing is a pure
     * reinterpret too.  Without this arm the generic CK_CARRIER->CK_CONCRETE
     * path below asks `carrier_is_inline(TY_APP)`, gets false, and emits a
     * struct deref-copy: `schan-recv`'s `(Pair T (SChan R))` construction
     * emitted `*(void **)(intptr_t)(chan)` and segfaulted (schan-roundtrip,
     * schan-worker-pool).  Before the seam the type never reached the bridge at
     * all -- it was carrier-ABI, so no crossing existed to get wrong. */
    if (adt_opaque_c_names_as_pointer(
            concrete_ty.kind == TY_ADT ? concrete_ty.as.adt_.def
          : concrete_ty.kind == TY_APP ? type_adt_app_def(&concrete_ty)
                                       : NULL)) {
        if (src_ck == CK_CARRIER && sink_ck == CK_CONCRETE)
            buf_printf(&out, "(%s)(intptr_t)(%s)", cname, src_str);
        else
            buf_printf(&out, "(int64_t)(intptr_t)(%s)", src_str);
        free(src_str);
        char *result = strdup(out.data);
        buf_free(&out);
        return result;
    }

    if (type_is_heap_struct(concrete_ty) || type_is_heap_adt(concrete_ty)) {
        if (src_ck == CK_CARRIER && sink_ck == CK_CONCRETE) {
            /* If the concrete C type is a pointer (`Vec__int *`), cast to it;
             * when abstract (cname collapses to int64_t) the carrier already
             * holds exactly what the sink wants -- pass through unchanged. */
            if (cname && strchr(cname, '*'))
                buf_printf(&out, "(%s)(intptr_t)(%s)", cname, src_str);
            else
                buf_printf(&out, "%s", src_str);
        } else {
            buf_printf(&out, "(int64_t)(intptr_t)(%s)", src_str);
        }
        free(src_str);
        char *result = strdup(out.data);
        buf_free(&out);
        return result;
    }

    /* carrier-bridge-pointer-leaf-dereferenced: a cstr / ptr<void> / sym (and
     * the int64 family) IS one carrier word -- the producer stores it with
     * `(int64_t)(intptr_t)`, so the crossing is a cast both ways.  These kinds
     * used to fall to the aggregate arms below: carrier->concrete read the
     * word as a POINTER TO a cstr (`*(const char **)w`, a segfault on the
     * first `(let [y (vec-get v 0)] y)` at A=cstr) and concrete->carrier
     * spilled the pointer to a stack temp and passed the temp's ADDRESS.
     * The M3 audit above already names these leaves "cross with no
     * reinterpret"; this arm is what makes that true. */
    /* A function value rides the carrier as its (fat-box or code) POINTER, so
     * it is the same leaf: `(mx-id f)` at A := (fn [int] int) dereferenced the
     * word as a boxed aggregate and called through garbage. */
    bool fn_leaf = concrete_ty.kind == TY_FN && cname && strchr(cname, '*') != NULL;
    switch (fn_leaf ? TY_PTR_VOID : concrete_ty.kind) {
        case TY_CSTR: case TY_PTR_VOID: case TY_SYM:
        case TY_INT: case TY_INT64: case TY_UINT64: {
            bool ptr = cname && strchr(cname, '*') != NULL;
            if (src_ck == CK_CARRIER && sink_ck == CK_CONCRETE)
                buf_printf(&out, ptr ? "((%s)(intptr_t)(%s))" : "((%s)(%s))",
                           cname ? cname : "int64_t", src_str);
            else
                buf_printf(&out, ptr ? "((int64_t)(intptr_t)(%s))"
                                     : "((int64_t)(%s))", src_str);
            free(src_str);
            char *result = strdup(out.data);
            buf_free(&out);
            return result;
        }
        default: break;
    }

    if (src_ck == CK_CARRIER && sink_ck == CK_CONCRETE) {
        if (carrier_is_inline(concrete_ty.kind)) {
            /* Inline scalar: union bitwise reinterpret int64_t -> concrete. */
            buf_printf(&out, "((union { int64_t s; %s d; }){.s = (%s)}).d",
                       cname, src_str);
        } else {
            /* decode-bool-carrier-instance-ascription:
             * The carrier source produced by inline-C `tur_box_ok` / `tur_box_some`
             * always lays out its payload fields as int64_t (the universal
             * `tur_result_box_t` / `tur_option_t` shape). The by-value sink
             * struct `Result__T__U` / `Option__T` lays them out at native
             * widths. The two layouts only coincide when every field's
             * resolved type is 8-byte-shaped (int, cstr, ptr, value-struct
             * with the lines 1049-1062 carve-out forcing a `T *` slot).
             *
             * For sub-word payloads (bool, int8/16/32, float32) a plain
             * `*(Result__T__U *)` deref reads padding bytes of the carrier
             * box. Reconstruct field-by-field from the canonical carrier
             * layout instead. The non-Result/Option fallback keeps the
             * legacy deref so unrelated parametric structs (Pair, user
             * defstructs) are unaffected. */
            /* structdef-retirement DS-D: the former struct-app Option/Result
             * canonical readback (keyed on type_extract_struct_app) is dead --
             * no struct-headed app forms.  The lowered record-ADT readback
             * below handles Option/Result monomorphs. */
            bool used_canonical = false;
            /* CONV-S1 seam 4: the same canonical NULL-safe readback for a LOWERED
             * Option/Result record ADT (`tur_adt_Option__Device`).  The struct
             * path above keys on type_extract_struct_app and misses an ADT app, so
             * a `none` (NULL carrier) would otherwise hit the unguarded deref below
             * and segfault.  Read the canonical box fields by name (the ctor field
             * names match tur_option_t/tur_result_box_t) and write the ADT
             * aggregate through its member path, guarding Option with a NULL test
             * that collapses to a zeroed `{0}` (is_some=false). */
            if (!used_canonical) {
                Type rty = emit_resolve_type(ctx, concrete_ty);
                AdtDef *adt = (rty.kind == TY_APP) ? type_adt_app_def(&rty)
                            : (rty.kind == TY_ADT ? rty.as.adt_.def : NULL);
                if (adt && adt->name && adt->n_ctors == 1 &&
                    adt->ctors[0]->is_record &&
                    (strcmp(adt->name, "Option") == 0 ||
                     strcmp(adt->name, "Result") == 0)) {
                    const CtorDef *ctor = adt->ctors[0];
                    bool is_option = strcmp(adt->name, "Option") == 0;
                    const char *box_t = is_option ? "tur_option_t"
                                                  : "tur_result_box_t";
                    char *src_tmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "%s *%s = (%s *)(intptr_t)(%s);\n",
                               box_t, src_tmp, box_t, src_str);
                    if (is_option) buf_printf(&out, "(%s ? ", src_tmp);
                    buf_printf(&out, "(%s){", cname);
                    for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
                        if (fi > 0) buf_puts(&out, ", ");
                        char *mp = adt_field_member_path(adt, ctor, fi);
                        char *bf = mangle_field_name(ctor->fields[fi].name);
                        Type field_ty = adt_field_type_for_app(&rty,
                                            &ctor->fields[fi]);
                        if (field_ty.kind == TY_UNKNOWN)
                            field_ty = type_simple(ctor->fields[fi].kind, CK_COPY);
                        const char *fcname = emit_type_c_name(ctx, field_ty);
                        TypeKind fk = field_ty.kind;
                        buf_printf(&out, ".%s = ", mp);
                        if (fk == TY_BOOL) {
                            buf_printf(&out, "%s->%s", src_tmp, bf);
                        } else if (fk == TY_INT || fk == TY_INT64 ||
                                   fk == TY_UINT64) {
                            buf_printf(&out, "%s->%s", src_tmp, bf);
                        } else if (fk == TY_FLOAT || fk == TY_FLOAT64 ||
                                   fk == TY_FLOAT32) {
                            buf_printf(&out,
                                "((union { int64_t s; %s d; }){.s = %s->%s}).d",
                                fcname, src_tmp, bf);
                        } else if (fk == TY_CSTR || fk == TY_PTR_VOID) {
                            buf_printf(&out, "(%s)(intptr_t)(%s->%s)",
                                       fcname, src_tmp, bf);
                        } else if (adt_field_is_ros_pointer_box(adt, &field_ty)) {
                            /* structdef-retirement slice 1: this Result/Option
                             * field is the `tur_adt_T *` pointer-box slot (a
                             * non-parametric value-struct or by-value record-ADT
                             * payload like `(Result Box cstr)`'s ok_val).  The
                             * monomorph literal's slot type IS the pointer, and the
                             * carrier box already holds the heap pointer as int64,
                             * so cast to the typed pointer -- do NOT deref (that
                             * would store a `T` value into a `T *` slot). */
                            buf_printf(&out, "(%s *)(intptr_t)(%s->%s)",
                                       fcname, src_tmp, bf);
                        } else if ((fk == TY_STRUCT || fk == TY_ADT ||
                                    fk == TY_APP) &&
                                   !type_is_heap_struct(field_ty) &&
                                   !type_is_heap_adt(field_ty) &&
                                   type_has_concrete_codegen_layout(&field_ty) &&
                                   fcname && strcmp(fcname, "int64_t") != 0) {
                            /* nested-construct-byvalue (Gap #4 / site 5): a WIDE
                             * by-value aggregate field (`(Result (Option int)
                             * cstr)`'s ok_val holding a parametric monomorph) is
                             * stored BOXED in the canonical carrier box (malloc'd
                             * pointer cast to int64), so a direct cast is an illegal
                             * int64->aggregate conversion.  Deref-unbox the heap
                             * pointer instead. */
                            buf_printf(&out, "(*(%s *)(intptr_t)(%s->%s))",
                                       fcname, src_tmp, bf);
                        } else {
                            buf_printf(&out, "(%s)(%s->%s)", fcname, src_tmp, bf);
                        }
                        free(mp); free(bf);
                    }
                    buf_printf(&out, "}");
                    /* S1/findings 16.4: cname can be scalar; emit_c_zero_of
                     * picks ((T)0) vs (T){0} so c2mir accepts either. */
                    if (is_option) {
                        char *z = emit_c_zero_of(cname);
                        buf_printf(&out, " : %s)", z);
                        free(z);
                    }
                    free(src_tmp);
                    used_canonical = true;
                }
            }
            if (!used_canonical) {
                /* SR2b + SR3 slice A: Option and Result are real SUMS now, so
                 * the canonical readback above (keyed on the pre-SR2b
                 * single-ctor RECORD form) no longer fires for them -- and it
                 * does not need to.  A carrier box for a sum monomorph IS that
                 * monomorph's layout (`{ int tag; union { ... } as; }`, which
                 * the preamble's tur_option_t / tur_result_box_t _Static_assert
                 * pins), so the readback is a plain deref.
                 *
                 * What does NOT survive the change is the NULL guard the record
                 * path carried.  The carrier None is the null pointer (slice A),
                 * so an Option-shaped monomorph must test before dereferencing
                 * and collapse to the zeroed aggregate -- tag 0 is None, the
                 * same answer `tur_is_some(0)` and the carrier match path's
                 * `__scrut ? __scrut->tag : 0` give.  Without it
                 * `(some? (:: (lookup 3) (Option int)))` derefs NULL, which is
                 * how the whole inline-C Option family crashed when SR2a went
                 * default (option-result-c-abi, inline-c-result-builder,
                 * inline-c-typed-result-option, inline-c-option-byval-param).
                 * Keyed by adt_ctor_is_null_none, the same shape check slice A
                 * uses on the producing side -- Result gets no guard because it
                 * has no null value to produce. */
                Type _rty = emit_resolve_type(ctx, concrete_ty);
                AdtDef *_adt = (_rty.kind == TY_APP) ? type_adt_app_def(&_rty)
                             : (_rty.kind == TY_ADT ? _rty.as.adt_.def : NULL);
                if (_adt && _adt->ctors && _adt->ctors[0] &&
                    adt_ctor_is_null_none(_adt, _adt->ctors[0])) {
                    /* Bind the carrier once: src_str may be a call. */
                    bool owns_box = emit_str_is_bare_ident(src_str) &&
                                    emit_owned_carrier_is(src_str);
                    char *ctmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "int64_t %s = (int64_t)(intptr_t)(%s);\n",
                               ctmp, src_str);
                    char *z = emit_c_zero_of(cname);
                    if (owns_box) {
                        /* inline-c-option-carrier-box-leaks: the box was
                         * malloc'd by an inline-C body and this readback is
                         * what consumes it.  The deref COPIES the contents
                         * into the aggregate, so once the copy is
                         * materialized the box is dead -- but the expression
                         * form below cannot free it (there is nowhere in a
                         * ternary to put a statement), which is exactly why
                         * it leaked.  Materialize into a temp, then free.
                         *
                         * The null test does double duty: `tur_none()` is the
                         * null carrier (SR3 slice A), so a None allocates
                         * nothing and must not be freed. */
                        char *vtmp = fresh_tmp(ctx);
                        if (!emit_readback_nested_sum_box(ctx, body, cname,
                                                          concrete_ty, ctmp, vtmp)) {
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s %s = (%s ? (*(%s *)(intptr_t)(%s)) : %s);\n",
                                   cname, vtmp, ctmp, cname, ctmp, z);
                        }
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "if (%s) free((void *)(intptr_t)(%s));\n",
                                   ctmp, ctmp);
                        buf_printf(&out, "%s", vtmp);
                        /* Consume the mark: a temp bridged twice must not be
                         * freed twice. */
                        emit_owned_carrier_clear(src_str);
                        free(vtmp);
                    } else {
                        /* A fresh inline-C box headed into an argument (see
                         * the generic deref below) whose payload nests a sum. */
                        if (emit_src_is_pending_sum_box(ctx, src_str) &&
                            emit_nested_sum_box_applies(ctx, concrete_ty)) {
                            char *nv = fresh_tmp(ctx);
                            emit_readback_nested_sum_box(ctx, body, cname,
                                                         concrete_ty, ctmp, nv);
                            buf_printf(&out, "%s", nv);
                            free(nv);
                        } else {
                            buf_printf(&out, "(%s ? (*(%s *)(intptr_t)(%s)) : %s)",
                                       ctmp, cname, ctmp, z);
                        }
                    }
                    free(z);
                    free(ctmp);
                } else if (emit_str_is_bare_ident(src_str) &&
                           emit_owned_carrier_is(src_str)) {
                    /* inline-c-option-carrier-box-leaks, the Result half.  Same
                     * consumption as the Option branch above, minus the null
                     * test: a Result has no null value to produce (both
                     * variants carry a payload, which is also why SR3's niche
                     * is Option-only), so the box is always there to free.
                     * `stdlib/result.tur`'s `tur_box_ok` in `result/bimap` is
                     * the in-tree site the report names for this branch. */
                    char *ctmp = fresh_tmp(ctx);
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "int64_t %s = (int64_t)(intptr_t)(%s);\n",
                               ctmp, src_str);
                    char *vtmp = fresh_tmp(ctx);
                    if (!emit_readback_nested_sum_box(ctx, body, cname,
                                                      concrete_ty, ctmp, vtmp)) {
                        indent_buf(body, ctx->indent);
                        buf_printf(body, "%s %s = (*(%s *)(intptr_t)(%s));\n",
                                   cname, vtmp, cname, ctmp);
                    }
                    indent_buf(body, ctx->indent);
                    buf_printf(body, "if (%s) free((void *)(intptr_t)(%s));\n",
                               ctmp, ctmp);
                    buf_printf(&out, "%s", vtmp);
                    emit_owned_carrier_clear(src_str);
                    free(vtmp);
                    free(ctmp);
                } else if (concrete_ty.kind == TY_ANY) {
                    /* saffron-dynamic-surface-pass H2: the carrier is 0 when a
                     * map accessor MISSED (`map-get`'s documented "0 (none) if
                     * absent"), and dereferencing that was a segfault on every
                     * `(Map K any)` miss -- i.e. every Saffron `#map{...}` miss.
                     * A missing `any` is the nil box, which is also what makes
                     * `(if (map-get m k) ...)` a presence test under Saffron
                     * truthiness.  The interpreter's map_val_read answers the
                     * same nil. */
                    /* A helper call, not a `({ ... })`: see
                     * ensure_any_carrier_bridge for the JIT x86-64 reason. */
                    ensure_any_carrier_bridge(ctx);
                    buf_printf(&out, "__tur_any_of_carrier((int64_t)(intptr_t)(%s))",
                               src_str);
                } else {
                    /* inline-c-builders-cannot-nest-option-in-result: a fresh
                     * inline-C sum box headed straight into an argument (RM1
                     * frees the CELL after the call, sum_pending) has the
                     * builders' layout, not the monomorph's, when its payload
                     * nests a sum.  Convert it field by field; the conversion
                     * frees the inner boxes, the pending drain the cell. */
                    char *nested_v = NULL;
                    if (emit_src_is_pending_sum_box(ctx, src_str) &&
                        emit_nested_sum_box_applies(ctx, concrete_ty)) {
                        {
                            nested_v = fresh_tmp(ctx);
                            if (!emit_readback_nested_sum_box(ctx, body, cname,
                                                              concrete_ty, src_str,
                                                              nested_v)) {
                                free(nested_v);
                                nested_v = NULL;
                            }
                        }
                    }
                    /* Pointer carrier: dereference the heap pointer -- NULL-safely
                     * for a sum whose nullary tag-0 value rides as 0 (Option's
                     * `none`; hkt-generic-none-to-typed-param-segfaults). */
                    const char *nullsafe = nested_v ? NULL :
                        ensure_agg_unbox_nullsafe(ctx, concrete_ty, cname);
                    if (nested_v) {
                        buf_printf(&out, "%s", nested_v);
                        free(nested_v);
                    } else if (nullsafe)
                        buf_printf(&out, "%s((int64_t)(intptr_t)(%s))", nullsafe,
                                   src_str);
                    else
                        buf_printf(&out, "(*(%s *)(intptr_t)(%s))", cname, src_str);
                }
            }
        }
    } else {
        /* CK_CONCRETE -> CK_CARRIER */
        if (carrier_is_inline(concrete_ty.kind)) {
            /* Inline scalar: union bitwise reinterpret concrete -> int64_t. */
            buf_printf(&out, "((union { %s s; int64_t d; }){.s = (%s)}).d",
                       cname, src_str);
        } else {
            /* Aggregate: spill to a local, return its address as int64_t.
             * The spill local uses a fresh tmp so it stays live through the
             * expression that consumes the carrier value. */
            char *tmp = fresh_tmp(ctx);
            indent_buf(body, ctx->indent);
            buf_printf(body, "%s %s = %s;\n", cname, tmp, src_str);
            buf_printf(&out, "(int64_t)(intptr_t)(&%s)", tmp);
            free(tmp);
        }
    }

    free(src_str);
    char *result = strdup(out.data);
    buf_free(&out);
    return result;
}

char *emit_carrier_bridge_escaping(EmitCtx *ctx, Buf *body,
                                   char *src_str,
                                   CarrierKind src_ck, CarrierKind sink_ck,
                                   Type concrete_ty) {
    /* Only the CK_CONCRETE -> CK_CARRIER aggregate spill differs from the
     * standard bridge; everything else is byte-for-byte identical, so delegate.
     *  - src_ck == sink_ck: no crossing.
     *  - carrier->concrete: a read-back, never takes an address.
     *  - :heap struct / :heap ADT: the pointer IS the carrier (shared by
     *    identity).  Under the defstruct->defadt lowering Vec/Map/Set/List are
     *    heap ADTs, not heap structs, so a nested heap-container element
     *    (`(Vec (Vec int))`) must delegate here too -- otherwise it falls into
     *    the aggregate heap-promote below and boxes the pointer into a `T **`.
     *  - inline scalar: a union reinterpret, no address taken.
     *  - pointer-sized leaf (cstr/ptr<void>/int*): already int64-compatible,
     *    no address taken.
     *  - niche `(Option P)`: the value IS a payload pointer, and the standard
     *    bridge's niche row (`p ? tur_box_some(p) : 0`) already heap-allocates
     *    the carrier box it hands back, so it escapes safely.  Falling into
     *    the aggregate heap-promote below instead boxed the payload pointer
     *    into a bare `P **` cell -- which the carrier->concrete reader then
     *    read as a tagged Option box, so the FIRST element of
     *    `(vec-of (some s) ...)` came back `(none)` while the second (routed
     *    through the standard bridge at a different site) was correct. */
    if (src_ck != CK_CONCRETE || sink_ck != CK_CARRIER ||
        type_is_heap_struct(concrete_ty) || type_is_heap_adt(concrete_ty) ||
        adt_app_is_niche_option(concrete_ty) ||
        carrier_is_inline(concrete_ty.kind)) {
        return emit_carrier_bridge(ctx, body, src_str, src_ck, sink_ck,
                                   concrete_ty);
    }
    switch (concrete_ty.kind) {
        case TY_CSTR: case TY_PTR_VOID:
        case TY_INT:  case TY_INT64: case TY_UINT64:
            return emit_carrier_bridge(ctx, body, src_str, src_ck, sink_ck,
                                       concrete_ty);
        default: break;
    }

    /* inline-c-carrier-producer-byval-container-element, the position the
     * report is named for.  The heap-promote below copies the value in hand
     * into a `T *` cell -- but an inline-C Option producer hands back the
     * CARRIER word, so `*tmp = <int64_t>` against an aggregate cell is
     * "incompatible types when assigning".  The niche row above escapes this
     * by delegating; the by-value monomorph had no such row and fell straight
     * into the promote.
     *
     * Normalize FIRST, promote second: bridge the carrier word to the
     * aggregate, then box the aggregate.  Keyed on the value's recorded
     * emitted spelling, so a value that already IS the aggregate -- every
     * pre-existing caller of this path -- is untouched and cannot be
     * double-bridged. */
    if (emit_str_is_bare_ident(src_str)) {
        const char *sty = emit_localvar_lookup_ctype(src_str);
        if (sty && strcmp(sty, "int64_t") == 0)
            src_str = emit_carrier_bridge(ctx, body, src_str,
                                          CK_CARRIER, CK_CONCRETE,
                                          concrete_ty);
    }

    /* By-value aggregate flowing into an escaping heap container: heap-promote
     * instead of spilling to a stack local.  malloc a copy and carry the heap
     * pointer; the element now outlives the producing frame, and the
     * carrier->concrete reader (which derefs the pointer) is unchanged. */
    const char *cname = emit_type_c_name(ctx, concrete_ty);
    char *tmp = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s *%s = (%s *)malloc(sizeof(%s));\n",
               cname, tmp, cname, cname);
    indent_buf(body, ctx->indent);
    buf_printf(body, "*%s = %s;\n", tmp, src_str);

    Buf out;
    buf_init(&out);
    buf_printf(&out, "(int64_t)(intptr_t)(%s)", tmp);
    free(tmp);
    free(src_str);
    char *result = strdup(out.data);
    buf_free(&out);
    return result;
}

/* ============================================================================
 * SYM1 (runtime-symbols-plan): per-TU interned-symbol codegen registry.
 *
 * Every distinct `:foo` referenced in a translation unit lowers to a single
 * static `struct __tur_sym` record in .rodata.  Two references to the same
 * keyword fold to the same record (and therefore the same pointer), so `:Sym`
 * equality is pointer equality and hashing is a single field load.
 *
 * The registry is keyed by the interned compile-time Symbol* identity (the
 * reader/symtab already shares one Symbol* across all `:foo` occurrences in a
 * unit), with a name-string fallback so independently-interned symbols with the
 * same text still fold.  Mangling percent-encodes non-identifier bytes so the
 * emitted C identifier is unique and ASCII-only even for punctuated keywords.
 * ============================================================================
 */
#include "hamt.h"   /* tur_hamt_hash_str -- precomputed xxHash64 per keyword */

typedef struct SymRecord {
    char         *name;    /* malloc'd owned copy of the name (see note) */
    uint32_t      len;     /* byte length, excluding NUL */
    uint64_t      hash;    /* precomputed xxHash64 of name */
    char         *cid;     /* malloc'd mangled C identifier, e.g. "__tur_sym_foo" */
} SymRecord;

static SymRecord *g_sym_records   = NULL;
static uint32_t   g_n_sym_records = 0;
static uint32_t   g_cap_sym_records = 0;
/* SYM5: set when this TU defines str->sym (i.e. sym-dynamic.tur was loaded),
 * which is the only thing that links the runtime intern table.  The seeding
 * constructor (which references tur_sym_register) is emitted only then, so a
 * program that uses literal :Sym values without str->sym never links the
 * table and never runs a startup constructor. */
static bool       g_sym_intern_used = false;

void sym_codegen_note_intern_used(void) { g_sym_intern_used = true; }

/* The name is strdup'd rather than borrowed from the source Symbol: in the
 * multi-file build path each TU is elaborated in its own arena which is freed
 * before the next TU, so a borrowed Symbol->name would dangle across the
 * registry's lifetime.  sym_codegen_reset() must still be called per TU to
 * keep dedup TU-local, but owning the string makes a missed reset a leak
 * rather than a use-after-free. */
void sym_codegen_reset(void) {
    for (uint32_t i = 0; i < g_n_sym_records; i++) {
        free(g_sym_records[i].cid);
        free(g_sym_records[i].name);
    }
    free(g_sym_records);
    g_sym_records     = NULL;
    g_n_sym_records   = 0;
    g_cap_sym_records = 0;
    g_sym_intern_used = false;
}

uint32_t sym_codegen_count(void) { return g_n_sym_records; }

/* Append the mangled form of `name` to `out`: each byte that is not [A-Za-z0-9_]
 * is emitted as `_HH` (uppercase hex of the byte), so the result is a valid,
 * collision-free C identifier suffix. */
static void sym_mangle_append(Buf *out, const char *name, uint32_t len) {
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            buf_putc(out, (char)c);
        } else {
            buf_putc(out, '_');
            buf_putc(out, hex[(c >> 4) & 0xF]);
            buf_putc(out, hex[c & 0xF]);
        }
    }
}

const char *sym_codegen_register(const Symbol *sym) {
    if (!sym) return NULL;
    /* Dedup by name text (the source Symbol* identity is not stable across the
     * per-TU arenas of a multi-file build). */
    for (uint32_t i = 0; i < g_n_sym_records; i++) {
        if (g_sym_records[i].len == sym->len &&
            strcmp(g_sym_records[i].name, sym->name) == 0)
            return g_sym_records[i].cid;
    }
    if (g_n_sym_records == g_cap_sym_records) {
        uint32_t nc = g_cap_sym_records ? g_cap_sym_records * 2 : 8;
        SymRecord *nr = (SymRecord *)realloc(g_sym_records, nc * sizeof(SymRecord));
        if (!nr) { fprintf(stderr, "tur: oom\n"); abort(); }
        g_sym_records = nr;
        g_cap_sym_records = nc;
    }
    Buf cid; buf_init(&cid);
    buf_puts(&cid, "__tur_sym_");
    sym_mangle_append(&cid, sym->name, sym->len);
    buf_putc(&cid, '\0');
    SymRecord *rec = &g_sym_records[g_n_sym_records++];
    rec->name = strdup(sym->name);
    rec->len  = sym->len;
    rec->hash = tur_hamt_hash_str(sym->name);
    rec->cid  = strdup(cid.data);
    buf_free(&cid);
    return rec->cid;
}

/* Emit the runtime symbol struct + one record per distinct keyword.
 * `struct __tur_sym` is emitted unconditionally when -Xsymbols is on (so that
 * inline-C in stdlib sym helpers always sees the layout), and also whenever any
 * record was registered.
 *
 * SYM2 (cross-TU interning): when `external_weak` is true (the multi-file /
 * separate-compilation build path), each record is emitted with external weak
 * linkage under its stable mangled name.  Two TUs that both reference `:foo`
 * emit identical `__tur_sym_foo` definitions; the linker folds the weak
 * duplicates to a single object, so `:foo` is one pointer across the whole
 * program.  In single-file / emit-c mode (`external_weak` false) the records
 * stay `static`, keeping the output self-contained. */
/* r7rs-prelude-split-wrong-symbols-on-windows: the records the LIBRARY unit
 * of a split build defined, by C name.  The client unit, emitted next in the
 * same process, declares these `extern` and defines only the rest. */
static char   **g_split_lib_syms;
static uint32_t g_n_split_lib_syms, g_cap_split_lib_syms;

void sym_codegen_split_clear(void) {
    for (uint32_t i = 0; i < g_n_split_lib_syms; i++) free(g_split_lib_syms[i]);
    free(g_split_lib_syms);
    g_split_lib_syms = NULL;
    g_n_split_lib_syms = g_cap_split_lib_syms = 0;
}

static bool split_lib_defines_sym(const char *cid) {
    for (uint32_t i = 0; i < g_n_split_lib_syms; i++)
        if (strcmp(g_split_lib_syms[i], cid) == 0) return true;
    return false;
}

static void split_lib_note_sym(const char *cid) {
    if (split_lib_defines_sym(cid)) return;
    if (g_n_split_lib_syms == g_cap_split_lib_syms) {
        uint32_t nc = g_cap_split_lib_syms ? g_cap_split_lib_syms * 2 : 64;
        char **nv = (char **)realloc(g_split_lib_syms, nc * sizeof(char *));
        if (!nv) { fprintf(stderr, "tur: oom\n"); abort(); }
        g_split_lib_syms = nv;
        g_cap_split_lib_syms = nc;
    }
    g_split_lib_syms[g_n_split_lib_syms++] = strdup(cid);
}

void sym_codegen_emit(Buf *out, bool external_weak) {
    buf_puts(out,
        "/* SYM1 (runtime-symbols-plan): interned runtime symbol records. */\n"
        "#ifndef TUR_SYM_DEFINED\n"
        "#define TUR_SYM_DEFINED 1\n"
        "struct __tur_sym {\n"
        "    uint64_t hash;   /* precomputed xxHash64 of name */\n"
        "    uint32_t len;    /* byte length, excluding NUL */\n"
        "    uint32_t _pad;\n"
        "    char     name[]; /* NUL-terminated UTF-8 */\n"
        "};\n"
        "#endif\n");
    /* SYM2 on PE/COFF (r7rs-prelude-split-wrong-symbols-on-windows): GNU ld
     * implements a weak DATA definition as a weak external plus a per-object
     * default, and a reference at an offset into it (`.name`) resolves to the
     * wrong bytes.  `selectany` is PE's own folding -- one COMDAT copy, every
     * reference correct -- so separately compiled modules use it there. */
    if (external_weak && g_emit_split == EMIT_SPLIT_NONE)
        buf_puts(out,
            "#ifndef TUR_SYM_LINKAGE\n"
            "#if defined(_WIN32) || defined(__CYGWIN__)\n"
            "#define TUR_SYM_LINKAGE __attribute__((selectany))\n"
            "#else\n"
            "#define TUR_SYM_LINKAGE __attribute__((weak))\n"
            "#endif\n"
            "#endif\n");
    const char *storage = external_weak
        ? "TUR_SYM_LINKAGE const"   /* SYM2: linker folds same-named dups */
        : "static const";
    /* r7rs-prelude-split-wrong-symbols-on-windows: a split build does not
     * lean on weak data.  PE/COFF has none that folds -- GNU ld gives each
     * object a weak external with its own default, so on Windows the two
     * units' `:caught` were two records, and a reference could land in
     * another object's string literals.  The library unit DEFINES each record
     * it uses (strong, external); the client unit declares those `extern` and
     * keeps a file-local record for a keyword only it quotes.  The SYM5 seed
     * still registers both units' records, first registration winning. */
    if (g_emit_split == EMIT_SPLIT_LIB) storage = "const";
    for (uint32_t i = 0; i < g_n_sym_records; i++) {
        SymRecord *r = &g_sym_records[i];
        if (g_emit_split == EMIT_SPLIT_LIB) {
            split_lib_note_sym(r->cid);
            emit_split_note_export(r->cid);
        } else if (g_emit_split == EMIT_SPLIT_CLIENT && split_lib_defines_sym(r->cid)) {
            buf_printf(out,
                "extern const struct { uint64_t hash; uint32_t len; uint32_t _pad; char name[%u]; } %s;\n",
                r->len + 1, r->cid);
            continue;
        }
        const char *st = (g_emit_split == EMIT_SPLIT_CLIENT) ? "static const" : storage;
        /* The record has a flexible array member, so use a sized anonymous
         * struct for the definition and cast to const struct __tur_sym * at use. */
        buf_printf(out,
            "%s struct { uint64_t hash; uint32_t len; uint32_t _pad; char name[%u]; } %s = { ",
            st, r->len + 1, r->cid);
        buf_printf(out, "%lluULL, %uu, 0u, ",
                   (unsigned long long)r->hash, r->len);
        /* Emit the name as a C string literal (escape conservatively). */
        buf_putc(out, '"');
        for (uint32_t j = 0; j < r->len; j++) {
            unsigned char c = (unsigned char)r->name[j];
            if (c == '"' || c == '\\') { buf_putc(out, '\\'); buf_putc(out, (char)c); }
            else if (c == '\n') buf_puts(out, "\\n");
            else if (c == '\t') buf_puts(out, "\\t");
            else if (c < 0x20)  buf_printf(out, "\\%03o", c);
            else                buf_putc(out, (char)c);
        }
        buf_puts(out, "\" };\n");
    }
    /* SYM5: seed the runtime intern table with this TU's static records so that
     * str->sym("foo") returns the same pointer as the literal :foo.  The
     * registrar lives in src/runtime/symbols.c, auto-linked into -Xsymbols
     * programs via the marker in stdlib/sym.tur's str->sym.  First registration
     * of a name wins, so weak-folded cross-TU records register idempotently.
     * Gated on str->sym being defined in this TU (g_sym_intern_used): only then
     * is the table (and tur_sym_register) linked, and only then can anything
     * query the table -- so a literal-only program emits no constructor.
     * Named outside the records' `__tur_sym_` prefix: a keyword `:seed` is
     * the record `__tur_sym_seed`. */
    /* r7rs-programs-compile-slowly: the program unit of a split build seeds
     * its records too -- str->sym lives in the library unit, and a symbol
     * only the program quotes must still be the one `string->symbol` finds. */
    if (g_n_sym_records > 0 && (g_sym_intern_used || g_emit_split == EMIT_SPLIT_CLIENT)) {
        buf_puts(out, "extern void tur_sym_register(const struct __tur_sym *);\n");
        buf_puts(out, "static void __tur_symtab_seed(void) {\n");
        for (uint32_t i = 0; i < g_n_sym_records; i++) {
            buf_printf(out, "    tur_sym_register((const struct __tur_sym *)&%s);\n",
                       g_sym_records[i].cid);
        }
        buf_puts(out, "}\n");
        static_init_register("__tur_symtab_seed", STATIC_INIT_REGISTRY);
    }
    if (g_n_sym_records > 0) buf_putc(out, '\n');
}

/* ============================================================================
 * S1b (jit-engine-plan): explicit static initialization.
 * ============================================================================
 *
 * Every startup action the emitter used to hang off
 * `__attribute__((constructor))` is registered here instead and called from an
 * explicit `__tur_static_init()` at the top of `main`.  The motivation is the
 * JIT: c2mir parses GCC attributes and discards them with NO diagnostic
 * (docs/archive/jit-engine-j0-findings.md section 3.1), so a dropped
 * `constructor` cost a SIGSEGV in effectful code and wrong output in dynamic
 * variables -- silently.  An ordinary call survives any C11 front end.
 *
 * A single `__attribute__((constructor))` wrapper is still emitted, so the
 * `cc` path keeps working exactly as before for the two cases an explicit call
 * from `main` cannot cover: separate compilation (each TU initializes itself,
 * and only one TU has `main`) and `--shared` libraries (no `main` at all).
 * `__tur_static_init` is therefore idempotent -- whichever of the two paths
 * fires first wins and the other is a no-op.
 *
 * Ordering was previously the toolchain's business (`.init_array` order within
 * a TU).  It is now ours, and the bands below encode the dependencies:
 * dynamic-variable pthread keys must exist before anything reads a dynamic
 * var, the registries must be populated before any effectful indirect call
 * dispatches through them, and `__tur_module_def_init` runs *user* code so it
 * goes last.
 */

typedef struct StaticInitEntry {
    char           *fn;    /* malloc'd owned copy of the C identifier */
    StaticInitBand  band;
} StaticInitEntry;

static StaticInitEntry *g_static_inits   = NULL;
static uint32_t         g_n_static_inits = 0;
static uint32_t         g_cap_static_inits = 0;

void static_init_reset(void) {
    for (uint32_t i = 0; i < g_n_static_inits; i++) free(g_static_inits[i].fn);
    free(g_static_inits);
    g_static_inits     = NULL;
    g_n_static_inits   = 0;
    g_cap_static_inits = 0;
}

void static_init_register(const char *fn, StaticInitBand band) {
    if (!fn || !*fn) return;
    for (uint32_t i = 0; i < g_n_static_inits; i++)
        if (strcmp(g_static_inits[i].fn, fn) == 0) return;   /* idempotent by name */
    if (g_n_static_inits == g_cap_static_inits) {
        uint32_t nc = g_cap_static_inits ? g_cap_static_inits * 2 : 8;
        StaticInitEntry *ne = (StaticInitEntry *)realloc(g_static_inits,
                                                         nc * sizeof(StaticInitEntry));
        if (!ne) return;
        g_static_inits     = ne;
        g_cap_static_inits = nc;
    }
    g_static_inits[g_n_static_inits].fn   = strdup(fn);
    g_static_inits[g_n_static_inits].band = band;
    g_n_static_inits++;
}

uint32_t static_init_count(void) { return g_n_static_inits; }

/* Emit the definition.  MUST come after every registered function's own
 * definition: they are all `static`, so a forward reference would be an
 * implicit declaration.  The declaration `main` calls is emitted in the
 * runtime preamble instead. */
void static_init_emit(Buf *out, bool gc_collector_pasted) {
    /* r7rs-programs-compile-slowly: the library unit of a split build has no
     * `main` and no constructor.  Its initializers -- its fat boxes' fills,
     * its `any` rows, its stdlib globals -- run from __tur_split_lib_init,
     * which the client unit's __tur_static_init calls first, so the prelude
     * is initialized before the program's own code runs, as in one unit.
     * The region-pool shutdown stays with the client: it is registered once
     * per program. */
    if (g_emit_split == EMIT_SPLIT_LIB) {
        buf_puts(out,
            "/* r7rs-programs-compile-slowly: this unit's initializers, run by the\n"
            " * program unit's __tur_static_init before its own. */\n"
            "void __tur_split_lib_init(void) {\n"
            "    static int __tur_split_lib_init_done = 0;\n"
            "    if (__tur_split_lib_init_done) return;\n"
            "    __tur_split_lib_init_done = 1;\n");
        for (int band = STATIC_INIT_KEYS; band <= STATIC_INIT_DEFS; band++)
            for (uint32_t i = 0; i < g_n_static_inits; i++)
                if (g_static_inits[i].band == (StaticInitBand)band)
                    buf_printf(out, "    %s();\n", g_static_inits[i].fn);
        buf_puts(out, "}\n\n");
        return;
    }
    const bool split_client = (g_emit_split == EMIT_SPLIT_CLIENT);
    if (split_client) buf_puts(out, "extern void __tur_split_lib_init(void);\n");
    buf_puts(out,
        "/* S1b: explicit static initialization -- see docs/archive/jit-engine-plan.md.\n"
        " * Called from main(); the constructor below covers the no-main cases\n"
        " * (separate compilation, --shared).  Whichever runs first wins. */\n"
        "static void __tur_static_init(void) {\n");
    if (g_n_static_inits > 0 || split_client) {
        buf_puts(out, "    static int __tur_static_init_done = 0;\n"
                      "    if (__tur_static_init_done) return;\n"
                      "    __tur_static_init_done = 1;\n");
        /* r7rs-prelude-split-gc-seam-on-macos: the runtime archive's allocator
         * hook, installed here as well as from the collector's own
         * `constructor(101)`.  That priority orders initializers only WITHIN
         * one object file on Mach-O -- across object files ld64 runs them in
         * link order -- and under the prelude split the collector's ARMED copy
         * is in the library unit, which links after the program unit.  So this
         * function ran first and its bands allocated archive memory with libc:
         * __tur_symtab_seed grew the symbol table, __tur_module_def_init built
         * a HAMT, and the collector could trace through neither block, so a
         * Scheme value kept only in a Turmeric map was freed under it.
         *
         * This is the earliest point that is always ahead of the unit's own
         * allocations, on every platform and in whichever unit has it, so it
         * goes before the atexit band as well as before the prelude's init.
         * Idempotent, and safe before the collector is initialized (see
         * tur_gc_install_rt_allocator).  A target where the collector compiles
         * out -- Windows, or `tur jit`, where TUR_GC_ON is 0 because the
         * engine's globals are not in the data segment the root scan walks --
         * gets the no-op stub, so this line costs nothing there. */
        if (gc_collector_pasted) buf_puts(out, "    tur_gc_install_rt_allocator();\n");
        /* RM3 R5 item 3: register the region-pool shutdown HERE and nowhere
         * else -- see emit_region_shutdown_atexit for why this is the only
         * place the atexit ordering actually holds. */
        emit_region_shutdown_atexit(out, 4);
        if (split_client) buf_puts(out, "    __tur_split_lib_init();\n");
        for (int band = STATIC_INIT_KEYS; band <= STATIC_INIT_DEFS; band++)
            for (uint32_t i = 0; i < g_n_static_inits; i++)
                if (g_static_inits[i].band == (StaticInitBand)band)
                    buf_printf(out, "    %s();\n", g_static_inits[i].fn);
    }
    buf_puts(out, "}\n");
    if (g_n_static_inits > 0 || split_client)
        buf_puts(out,
            "__attribute__((constructor))\n"
            "static void __tur_static_init_ctor(void) { __tur_static_init(); }\n");
    buf_putc(out, '\n');
}


/* ========================================================================= *
 * byvalue-recursive-adt-boxes-are-never-freed: ownership PROVENANCE.
 *
 * A by-value recursive ADT (`(defdata Lst [] (Cons [hd : int tl : Lst])
 * (Nil))`, or one with an `any` field) owns a spine of heap boxes, and two
 * things free that spine: a let-local's scope-exit drop (`drop_localowned_<T>`,
 * since 2026-09-07) and a consuming callee's discharge of its parameter (the
 * "own half", below).  Both are sound only for a value that really OWNS its
 * spine, and the move checker does not guarantee that a by-value copy does:
 *
 *   (defn id-b [^borrow x : Lst] : Lst x)       ; a copy of a borrow
 *   (defn get-g [] : Lst g)                     ; a copy of a global
 *   (vec-get v 0)                               ; a copy of an element
 *
 * each hands back a value that SHARES its boxes with an owner that will free
 * them, and `(let [w (id-b zs)] (llen w))` freed zs's spine under zs
 * (measured: heap-use-after-free, for all three).
 *
 * So both consumers now ask this analysis first.  A value is OWNED when it is
 *   - a constructor call whose recursive-type arguments are owned;
 *   - a let-local or match binder proven owned here (binders inherit from an
 *     owned scrutinee); or a parameter whose every caller passes an owned
 *     value (OwnedParam, below);
 *   - the result of a call to a user function whose result is FRESH -- every
 *     value it can return is owned;
 *   - an if / match / do / let whose value positions are all owned.
 * Anything else -- a ^borrow parameter, a global, a field read, an indirect
 * call, a call into inline C or a stdlib accessor -- is not.
 *
 * OwnedParam and Fresh are a greatest fixed point over the whole program:
 * everything starts assumed, and a call site passing an unowned argument, a
 * function whose address is taken (its callers are unknown), an export, or an
 * unowned return value knocks a fact out until nothing changes.  The walk
 * enumerates operands through cps_visit_children, so no expression kind is
 * silently skipped, and descends into closure bodies itself.
 *
 * Refusing costs a leak; a wrong "owned" costs a use-after-free. */

typedef struct {
    const FnDef *fd;
    uint32_t     owned;        /* params (bit i) every caller passes owned */
    bool         fresh;        /* every returned value is owned */
    bool         addr_taken;   /* referenced as a value: callers unknown */
} OwnFn;

typedef struct {
    const void **keys;
    uint32_t    *vals;
    uint32_t     cap, n;
} OwnMap;

static uint32_t own_ptr_hash(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x;
}

static void own_map_free(OwnMap *m) {
    free(m->keys); free(m->vals);
    memset(m, 0, sizeof(*m));
}

static void own_map_clear(OwnMap *m) {
    if (m->keys) memset(m->keys, 0, m->cap * sizeof(*m->keys));
    m->n = 0;
}

static void own_map_put(OwnMap *m, const void *k, uint32_t v);
static void own_map_grow(OwnMap *m) {
    OwnMap old = *m;
    m->cap = old.cap ? old.cap * 2 : 256;
    m->keys = (const void **)calloc(m->cap, sizeof(*m->keys));
    m->vals = (uint32_t *)calloc(m->cap, sizeof(*m->vals));
    if (!m->keys || !m->vals) { fprintf(stderr, "tur: oom\n"); abort(); }
    m->n = 0;
    for (uint32_t i = 0; i < old.cap; i++)
        if (old.keys[i]) own_map_put(m, old.keys[i], old.vals[i]);
    free(old.keys); free(old.vals);
}

static void own_map_put(OwnMap *m, const void *k, uint32_t v) {
    if (!k) return;
    if ((m->n + 1) * 2 > m->cap) own_map_grow(m);
    uint32_t mask = m->cap - 1;
    for (uint32_t s = own_ptr_hash(k) & mask;; s = (s + 1) & mask) {
        if (!m->keys[s]) { m->keys[s] = k; m->vals[s] = v; m->n++; return; }
        if (m->keys[s] == k) { m->vals[s] = v; return; }
    }
}

static bool own_map_get(const OwnMap *m, const void *k, uint32_t *v) {
    if (!k || !m->cap) return false;
    uint32_t mask = m->cap - 1;
    for (uint32_t s = own_ptr_hash(k) & mask; m->keys[s]; s = (s + 1) & mask)
        if (m->keys[s] == k) { if (v) *v = m->vals[s]; return true; }
    return false;
}

static const Expr *g_own_prog;
static OwnFn      *g_own_fns;
static uint32_t    g_own_n;
static OwnMap      g_own_fn_ix;     /* fn Binding -> index into g_own_fns */
static OwnMap      g_own_set;       /* let-locals / binders proven owned */
static OwnFn      *g_own_cur;       /* function whose body is being walked */
static bool        g_own_changed;

/* The ADTs whose spines these consumers free: by-value, non-generic, with a
 * direct self-recursive box or an `any` field.  Mirrors the elaborator's
 * elab_byval_localowned_adt, which is what sets drops_local_owned. */
const AdtDef *emit_own_adt_of(Type t) {
    const AdtDef *def = NULL;
    if (t.kind == TY_ADT)      def = t.as.adt_.def;
    else if (t.kind == TY_APP) def = type_adt_app_def(&t);
    else                       return NULL;
    if (!def || def->is_heap || !def->needs_drop_glue) return NULL;
    if (def->n_type_params != 0) return NULL;
    for (uint32_t ci = 0; ci < def->n_ctors; ci++)
        for (uint32_t fi = 0; fi < def->ctors[ci]->n_fields; fi++)
            if (def->ctors[ci]->fields[fi].drop_inner_def == def ||
                def->ctors[ci]->fields[fi].kind == TY_ANY)
                return def;
    return NULL;
}

static OwnFn *own_fn_for(const Binding *b) {
    uint32_t ix;
    if (b && own_map_get(&g_own_fn_ix, b, &ix)) return &g_own_fns[ix];
    return NULL;
}

static int own_param_index(const FnDef *fd, const Binding *b) {
    if (!fd || !b) return -1;
    for (uint32_t i = 0; i < fd->n_params; i++)
        if (fd->params[i] == b) return (int)i;
    return -1;
}

static const Expr *own_peel(const Expr *e) {
    while (e && (e->kind == EX_ASCRIBE || e->kind == EX_CAST))
        e = e->kind == EX_ASCRIBE ? e->as.ascribe_.inner : e->as.cast_.expr;
    return e;
}

static bool own_expr(const Expr *e) {
    e = own_peel(e);
    if (!e) return false;
    switch (e->kind) {
        case EX_VAR: {
            const Binding *b = e->as.var.binding;
            if (!b || b->is_global || b->is_borrow) return false;
            if (own_map_get(&g_own_set, b, NULL)) return true;
            if (g_own_cur) {
                int i = own_param_index(g_own_cur->fd, b);
                if (i >= 0 && i < 32) return (g_own_cur->owned >> i) & 1u;
            }
            return false;
        }
        case EX_CALL: {
            if (e->as.call_.ctor) {
                for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                    const Expr *a = e->as.call_.args[i];
                    if (a && emit_own_adt_of(a->type) && !own_expr(a)) return false;
                }
                return true;
            }
            if (e->as.call_.fn_expr || !e->as.call_.fn_binding) return false;
            const OwnFn *f = own_fn_for(e->as.call_.fn_binding);
            return f && f->fresh;
        }
        case EX_IF:
            return e->as.if_.else_or_null &&
                   own_expr(e->as.if_.then_) && own_expr(e->as.if_.else_or_null);
        case EX_MATCH:
            if (e->as.match_.n_arms == 0) return false;
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                const Expr *ab = e->as.match_.arms[i].body;
                if (ab && ab->type.kind == TY_NEVER) continue;
                if (!own_expr(ab)) return false;
            }
            return true;
        case EX_DO:
            return e->as.do_.n > 0 && own_expr(e->as.do_.items[e->as.do_.n - 1]);
        case EX_LET:
        case EX_LETREC:
            return own_expr(e->as.let_.body);
        default:
            return false;
    }
}

static void own_walk(const Expr *e);
static bool own_walk_cb(const Expr *c, void *ud) { (void)ud; own_walk(c); return false; }

static void own_walk_nested_fn(const FnDef *fd) {
    if (!fd || !fd->body) return;
    OwnFn *saved = g_own_cur;
    /* A nested fn's own params are called through a value: never owned. */
    g_own_cur = fd->binding ? own_fn_for(fd->binding) : NULL;
    if (g_own_cur && g_own_cur->fd != fd) g_own_cur = NULL;
    own_walk(fd->body);
    g_own_cur = saved;
}

static void own_walk(const Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EX_VAR: {
            OwnFn *f = own_fn_for(e->as.var.binding);
            if (f && !f->addr_taken) { f->addr_taken = true; g_own_changed = true; }
            return;
        }
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++) {
                const Expr *init = e->as.let_.bindings[i].init;
                own_walk(init);
                const Binding *lb = e->as.let_.bindings[i].binding;
                if (lb && emit_own_adt_of(lb->type) && own_expr(init))
                    own_map_put(&g_own_set, lb, 1);
            }
            own_walk(e->as.let_.body);
            return;
        case EX_MATCH: {
            const Expr *sc = e->as.match_.scrutinee;
            own_walk(sc);
            bool os = sc && emit_own_adt_of(sc->type) && own_expr(sc);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                const MatchArm *arm = &e->as.match_.arms[i];
                if (os) {
                    const MatchPattern *pt = &arm->pattern;
                    for (uint32_t j = 0; j < pt->n_bindings; j++)
                        if (pt->bindings[j] && emit_own_adt_of(pt->bindings[j]->type))
                            own_map_put(&g_own_set, pt->bindings[j], 1);
                    if (pt->is_var && pt->var_binding)
                        own_map_put(&g_own_set, pt->var_binding, 1);
                }
                own_walk(arm->guard);
                own_walk(arm->body);
            }
            return;
        }
        case EX_CALL: {
            own_walk(e->as.call_.fn_expr);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                own_walk(e->as.call_.args[i]);
            own_walk(e->as.call_.dict_arg);
            if (!e->as.call_.fn_expr && e->as.call_.fn_binding) {
                OwnFn *f = own_fn_for(e->as.call_.fn_binding);
                if (f && f->owned) {
                    for (uint32_t i = 0; i < e->as.call_.n_args && i < 32; i++) {
                        if (!((f->owned >> i) & 1u)) continue;
                        if (!own_expr(e->as.call_.args[i])) {
                            f->owned &= ~(1u << i);
                            g_own_changed = true;
                        }
                    }
                    /* Fewer args than params (a partial application or a
                     * variadic tail): the rest arrive from an unknown place. */
                    if (e->as.call_.n_args < f->fd->n_params) {
                        uint32_t keep = e->as.call_.n_args >= 32
                            ? 0xffffffffu : ((1u << e->as.call_.n_args) - 1u);
                        if (f->owned & ~keep) { f->owned &= keep; g_own_changed = true; }
                    }
                }
            }
            return;
        }
        case EX_RETURN:
            own_walk(e->as.return_.value);
            if (g_own_cur && g_own_cur->fresh && !own_expr(e->as.return_.value)) {
                g_own_cur->fresh = false;
                g_own_changed = true;
            }
            return;
        case EX_CLOSURE:
            if (e->as.closure_.closure) own_walk_nested_fn(e->as.closure_.closure->fn);
            return;
        case EX_FN:
            own_walk_nested_fn(e->as.fn_.fn);
            return;
        case EX_FN_DEF:
            own_walk_nested_fn(e->as.fn_def_.fn);
            return;
        default:
            cps_visit_children(e, own_walk_cb, NULL);
            return;
    }
}

static void own_compute(EmitCtx *ctx, const Expr *prog) {
    if (g_own_prog == prog && g_own_fns) return;
    free(g_own_fns); g_own_fns = NULL; g_own_n = 0;
    own_map_free(&g_own_fn_ix);
    own_map_free(&g_own_set);
    g_own_prog = prog;
    if (!prog || prog->kind != EX_PROGRAM) return;

    uint32_t np = 0;
    const Expr **items = flatten_program_items(prog, &np);
    g_own_fns = (OwnFn *)calloc(np ? np : 1, sizeof(OwnFn));
    for (uint32_t i = 0; i < np; i++) {
        const Expr *it = items[i];
        if (!it || it->kind != EX_FN_DEF || !it->as.fn_def_.fn) continue;
        const FnDef *fd = it->as.fn_def_.fn;
        if (!fd->binding || own_fn_for(fd->binding)) continue;
        OwnFn *f = &g_own_fns[g_own_n];
        f->fd = fd;
        const Binding *fb = fd->binding;
        bool external = fb->is_instance_method || fb->c_export_name ||
                        fb->retain_c_linkage || fb->is_lifted_lambda ||
                        (ctx && ctx->separate_compilation && fb->is_exported) ||
                        (fb->name && strcmp(fb->name->name, "main") == 0);
        for (uint32_t pi = 0; pi < fd->n_params && pi < 32; pi++) {
            const Binding *p = fd->params[pi];
            if (external || !p || p->is_borrow || !emit_own_adt_of(p->type)) continue;
            if (fb->nonretain_ptr_param_mask & (1u << pi)) continue;   /* lent */
            f->owned |= (1u << pi);
        }
        f->fresh = fd->body && emit_own_adt_of(fd->body->type);
        own_map_put(&g_own_fn_ix, fb, g_own_n);
        g_own_n++;
    }

    /* Greatest fixed point: every fact only ever turns off. */
    for (int iter = 0; iter < 64; iter++) {
        g_own_changed = false;
        own_map_clear(&g_own_set);
        for (uint32_t i = 0; i < np; i++) {
            const Expr *it = items[i];
            if (!it) continue;
            if (it->kind == EX_FN_DEF && it->as.fn_def_.fn) {
                const FnDef *fd = it->as.fn_def_.fn;
                g_own_cur = fd->binding ? own_fn_for(fd->binding) : NULL;
                if (g_own_cur && g_own_cur->fd != fd) g_own_cur = NULL;
                own_walk(fd->body);
                if (g_own_cur && g_own_cur->fresh && !own_expr(fd->body)) {
                    g_own_cur->fresh = false;
                    g_own_changed = true;
                }
            } else {
                g_own_cur = NULL;
                own_walk(it);
            }
        }
        g_own_cur = NULL;
        for (uint32_t k = 0; k < g_own_n; k++) {
            if (g_own_fns[k].addr_taken && g_own_fns[k].owned) {
                g_own_fns[k].owned = 0;
                g_own_changed = true;
            }
        }
        if (!g_own_changed) { free(items); return; }
    }
    /* No fixed point inside the budget: claim nothing. */
    for (uint32_t k = 0; k < g_own_n; k++) { g_own_fns[k].owned = 0; g_own_fns[k].fresh = false; }
    own_map_clear(&g_own_set);
    free(items);
}

bool emit_own_binding_owned(EmitCtx *ctx, const Binding *b) {
    if (!ctx || !b) return false;
    own_compute(ctx, ctx->program_root);
    return own_map_get(&g_own_set, b, NULL);
}

/* byvalue-recursive-shared-copies-leak: is `e` a value that owns its spine --
 * a construction, a fresh call, an owned local -- by the analysis above?  The
 * question a caller asks before freeing a TEMPORARY it lent to a callee. */
bool emit_own_expr_owned(EmitCtx *ctx, const Expr *e) {
    if (!ctx || !e) return false;
    own_compute(ctx, ctx->program_root);
    OwnFn *saved = g_own_cur;
    g_own_cur = NULL;
    if (ctx->own_cur_fn && ctx->own_cur_fn->binding) {
        g_own_cur = own_fn_for(ctx->own_cur_fn->binding);
        if (g_own_cur && g_own_cur->fd != ctx->own_cur_fn) g_own_cur = NULL;
    }
    bool r = own_expr(e);
    g_own_cur = saved;
    return r;
}

/* byvalue-recursive-shared-copies-leak: may the caller free argument `i` of
 * `call` -- spilled to a temporary for a `const T *` formal -- right after the
 * call returns?  A fresh by-value recursive value (a construction, a fresh
 * call) handed straight to a parameter the callee neither keeps nor frees
 * (nonretaining, or a ^borrow kept only in a result that here is a scalar) had
 * no owner at all: `(llen (build 3))` leaked the whole spine on every call.
 * The result must be a non-pointer scalar, so nothing the call returns can
 * point into what is freed; a tail call keeps its jump and leaks as before. */
bool emit_call_arg_temp_is_lent_fresh_spine(EmitCtx *ctx, const Expr *call,
                                            uint32_t i) {
    if (!ctx || !call || call->kind != EX_CALL) return false;
    if (i >= call->as.call_.n_args || i >= 32) return false;
    if (call->as.call_.fn_expr || call->as.call_.wants_tailcall) return false;
    const Binding *fb = call->as.call_.fn_binding;
    if (!fb || !call_dispatch_is_static(call)) return false;
    if (bc_kind_can_alias(call->type.kind)) return false;
    const Expr *a = own_peel(call->as.call_.args[i]);
    if (!a || a->kind == EX_VAR) return false;       /* a local owns it */
    Type at = call->as.call_.args[i]->type;
    if (at.kind != TY_ADT || !emit_own_adt_of(at)) return false;
    if (!(((fb->nonretain_ptr_param_mask | fb->resalias_param_mask) >> i) & 1u))
        return false;
    return emit_own_expr_owned(ctx, call->as.call_.args[i]);
}

/* `drop_localowned_tur_adt_<T>((void *)&lv);` for a by-value recursive `t`. */
char *emit_localowned_drop_call(Type t, const char *lv) {
    char *mn = mangle_adt_name(t.as.adt_.def->name);
    size_t n = strlen(mn) + strlen(lv) + 64;
    char *s = (char *)malloc(n);
    if (!s) { fprintf(stderr, "tur: oom\n"); abort(); }
    snprintf(s, n, "drop_localowned_tur_adt_%s((void *)&%s);", mn, lv);
    free(mn);
    return s;
}

bool emit_own_param_owned(EmitCtx *ctx, const FnDef *fd, uint32_t i) {
    if (!ctx || !fd || !fd->binding || i >= 32) return false;
    own_compute(ctx, ctx->program_root);
    const OwnFn *f = own_fn_for(fd->binding);
    return f && f->fd == fd && ((f->owned >> i) & 1u);
}

/* ---- the own half: a consuming callee discharges its parameter ---------- *
 *
 * A caller hands a by-value recursive ADT to a callee it cannot prove
 * non-retaining by MOVING it (Residue 1 lends only to proven non-retaining
 * callees), and nothing discharged it there: `(defn tail [xs : Lst] : Lst
 * (match xs (Cons h t) t (Nil) (Nil)))` returned the copy of the second box's
 * contents and dropped the second box itself -- one leaked box per call.
 *
 * When a parameter is OWNED (every caller passes an owned value, above) and
 * the body's one use of it is a `match` that runs once per call, each arm
 * frees, after its value is computed, the boxes of the parameter's spine it
 * did not pass on:
 *   SHALLOW -- a recursive-field binder MOVED out exactly once (returned, or
 *              handed to a constructor or a consuming callee): its contents
 *              were copied on, so the box that held them is garbage;
 *   DEEP    -- a binder unused, or used only in ways the strict alias walk
 *              proves confined: the whole sub-spine is garbage.
 * Anything else discharges nothing for that field (a leak, as before).  The
 * parameter's own storage is the caller's and is never touched. */

static uint32_t g_own_count_n;
static const Binding *g_own_count_b;
static bool own_count_cb(const Expr *c, void *ud);
static void own_count(const Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EX_VAR:
            if (e->as.var.binding == g_own_count_b) g_own_count_n++;
            return;
        case EX_CLOSURE:
            if (e->as.closure_.closure) {
                const struct Closure *cl = e->as.closure_.closure;
                for (uint8_t i = 0; i < cl->n_captures; i++)
                    if (cl->captures[i] == g_own_count_b) g_own_count_n++;
                if (cl->fn) own_count(cl->fn->body);
            }
            return;
        case EX_FN:
            if (e->as.fn_.fn) own_count(e->as.fn_.fn->body);
            return;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn) own_count(e->as.fn_def_.fn->body);
            return;
        case EX_CALL:
            own_count(e->as.call_.fn_expr);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                own_count(e->as.call_.args[i]);
            own_count(e->as.call_.dict_arg);
            return;
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                own_count(e->as.let_.bindings[i].init);
            own_count(e->as.let_.body);
            return;
        case EX_MATCH:
            own_count(e->as.match_.scrutinee);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                own_count(e->as.match_.arms[i].guard);
                own_count(e->as.match_.arms[i].body);
            }
            return;
        default:
            cps_visit_children(e, own_count_cb, NULL);
            return;
    }
}
static bool own_count_cb(const Expr *c, void *ud) { (void)ud; own_count(c); return false; }

static uint32_t own_uses(const Expr *e, const Binding *b) {
    g_own_count_b = b;
    g_own_count_n = 0;
    own_count(e);
    return g_own_count_n;
}

/* Is `m` on the body's straight-line spine -- evaluated exactly once per
 * call, as the value of the body? */
static bool own_on_spine(const Expr *body, const Expr *m) {
    const Expr *cur = own_peel(body);
    while (cur) {
        if (cur == m) return true;
        if (cur->kind == EX_DO && cur->as.do_.n > 0)
            cur = own_peel(cur->as.do_.items[cur->as.do_.n - 1]);
        else if (cur->kind == EX_LET || cur->kind == EX_LETREC)
            cur = own_peel(cur->as.let_.body);
        else
            return false;
    }
    return false;
}

/* The one occurrence of `b` is a MOVE into a fresh copy, on the arm's
 * unconditional spine: a constructor argument, or an argument of a direct
 * call to a callee whose parameter consumes (not ^borrow, not proven
 * non-retaining).  `top` is the arm body itself: a call there is a tail call,
 * and freeing after it would cost the C compiler the jump a recursive walker
 * relies on, so it is declined (a leak, as before). */
static bool own_moved_once(const Expr *e, const Binding *b, bool top) {
    e = own_peel(e);
    if (!e) return false;
    switch (e->kind) {
        case EX_CALL: {
            const Binding *fb = e->as.call_.fn_binding;
            for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
                const Expr *a = own_peel(e->as.call_.args[i]);
                if (a && a->kind == EX_VAR && a->as.var.binding == b) {
                    if (e->as.call_.ctor) return true;
                    if (top || e->as.call_.fn_expr || !fb) return false;
                    const OwnFn *f = own_fn_for(fb);
                    if (!f || i >= f->fd->n_params || i >= 32) return false;
                    if (f->fd->params[i]->is_borrow) return false;
                    if (fb->nonretain_ptr_param_mask & (1u << i)) return false;
                    return true;
                }
                if (own_moved_once(a, b, false)) return true;
            }
            return false;
        }
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                if (own_moved_once(e->as.builtin.args[i], b, false)) return true;
            return false;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                if (own_moved_once(e->as.do_.items[i], b, top && i + 1 == e->as.do_.n))
                    return true;
            return false;
        case EX_LET:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                if (own_moved_once(e->as.let_.bindings[i].init, b, false)) return true;
            return own_moved_once(e->as.let_.body, b, top);
        default:
            return false;
    }
}

bool emit_own_match_discharges(EmitCtx *ctx, const Expr *m) {
    if (!ctx || !m || m->kind != EX_MATCH || !ctx->own_cur_fn) return false;
    if (getenv("TUR_NO_OWN_DISCHARGE")) return false;
    const FnDef *fd = ctx->own_cur_fn;
    const Expr *sc = own_peel(m->as.match_.scrutinee);
    if (!sc || sc->kind != EX_VAR) return false;
    const Binding *p = sc->as.var.binding;
    if (!emit_own_adt_of(p->type)) return false;
    int pi = own_param_index(fd, p);
    if (pi < 0 || !emit_own_param_owned(ctx, fd, (uint32_t)pi)) return false;
    if (!own_on_spine(fd->body, m)) return false;
    return own_uses(fd->body, p) == 1;
}

int emit_own_arm_field_discharge(EmitCtx *ctx, const Expr *m, const MatchArm *arm,
                                 uint32_t fi) {
    (void)m;
    const MatchPattern *pt = &arm->pattern;
    if (arm->guard || pt->is_wildcard || pt->is_var || pt->is_literal || !pt->ctor)
        return EMIT_OWN_NONE;
    const CtorDef *ctor = pt->ctor;
    if (fi >= ctor->n_fields || ctor->fields[fi].drop_inner_def != ctor->adt)
        return EMIT_OWN_NONE;
    const Binding *b = fi < pt->n_bindings ? pt->bindings[fi] : NULL;
    uint32_t n = b ? own_uses(arm->body, b) : 0;
    if (n == 0) return EMIT_OWN_DEEP;
    const Expr *body = own_peel(arm->body);
    if (body && body->kind == EX_VAR && body->as.var.binding == b)
        return EMIT_OWN_SHALLOW;
    own_compute(ctx, ctx->program_root);
    if (n == 1 && own_moved_once(arm->body, b, true)) return EMIT_OWN_SHALLOW;
    bool scalar = arm->body && !bc_kind_can_alias(arm->body->type.kind);
    if (localowned_binding_is_confined(arm->body, b, scalar)) return EMIT_OWN_DEEP;
    return EMIT_OWN_NONE;
}

void emit_own_arm_discharges(EmitCtx *ctx, Buf *body, const Expr *m,
                             const MatchArm *arm, const char *acc) {
    const MatchPattern *pt = &arm->pattern;
    if (!pt->ctor) return;
    const CtorDef *ctor = pt->ctor;
    const AdtDef *def = ctor->adt;
    for (uint32_t fi = 0; fi < ctor->n_fields; fi++) {
        if (ctor->fields[fi].drop_inner_def != def) continue;
        int mode = emit_own_arm_field_discharge(ctx, m, arm, fi);
        if (mode == EMIT_OWN_NONE) continue;
        char *mp = adt_field_member_path(def, ctor, fi);
        indent_buf(body, ctx->indent);
        if (mode == EMIT_OWN_SHALLOW) {
            buf_printf(body,
                "tur_region_free((void *)(intptr_t)__scrut%s%s);  /* moved out: free its box */\n",
                acc, mp);
        } else {
            char *mn = mangle_adt_name(def->name);
            buf_printf(body,
                "if (__scrut%s%s) drop_glue_tur_adt_%s((void *)(intptr_t)__scrut%s%s);  /* unused: free the sub-spine */\n",
                acc, mp, mn, acc, mp);
            free(mn);
        }
        free(mp);
    }
}
