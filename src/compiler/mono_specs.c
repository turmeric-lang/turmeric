/* mono_specs.c -- VBM1/VBM2 by-value HKT monomorphization spec registry.
 *
 * See mono_specs.h and docs/archive/history/van-laarhoven-monomorphization-plan.md.
 *
 * VBM1 populates a flat, deduped table of ABSTRACT lens specialization keys
 * during elaboration (each keyed on the abstract lens param `l` at the `(l g s)`
 * pin inside the enclosing fn).  VBM2 (`mono_specs_resolve_program`) walks the
 * elaborated program and resolves each abstract key to the CONCRETE lens defn
 * passed at every top-level call of its enclosing fn, recording a concrete key.
 * `--dump-mono-specs` prints both tables for review before VBM2b wires the
 * per-spec by-value body emit. */
#include "mono_specs.h"
#include "expr.h"

#include <stdint.h>
#include <string.h>

#define MONO_SPEC_FIELD 96
#define MONO_SPEC_MAX   1024

/* CM1 (van-laarhoven-consumer-mono-plan): the resolve pass no longer collapses a
 * consumer lens param to a single unique/ambiguous verdict.  It builds the full
 * SET of concrete lenses the param is reached with across the call graph.  Caps
 * bound the fixpoint; a hit is a `log`-style stderr note (never a silent
 * truncation) and the over-cap lenses/sites stay on Path A. */
#define MONO_SPEC_LENSSET_MAX     32   /* distinct concrete lenses per lens param */
#define MONO_SPEC_SITES_MAX       64   /* recorded call sites per (param, lens) */
#define MONO_SPEC_FIXPOINT_PASSES 64   /* fixpoint iteration backstop */

/* One resolved concrete lens for a consumer lens param: the concrete lens FnDef,
 * its `<lens>__mono_<hash>`, and every call-site `Expr *` that passes it (the
 * sites CM3 rewrites).  Sites are deduped by pointer identity. */
typedef struct {
    const void *lens_fn;                 /* concrete lens `const FnDef *` (may be NULL) */
    char        lens_name[MONO_SPEC_FIELD];
    uint64_t    mono_hash;               /* the `<lens>__mono_<hash>` emit key */
    const void *sites[MONO_SPEC_SITES_MAX]; /* call-site `const Expr *` passing this lens */
    size_t      n_sites;
    bool        sites_capped;
} LensResolution;

/* An ABSTRACT spec key registered by VBM1 at the `(l g s)` pin. */
typedef struct {
    char     enclosing[MONO_SPEC_FIELD];
    char     callee[MONO_SPEC_FIELD];    /* the abstract lens param name (`l`) */
    char     functor[MONO_SPEC_FIELD];
    char     focus[MONO_SPEC_FIELD];
    char     whole[MONO_SPEC_FIELD];
    char     tyvar[MONO_SPEC_FIELD];     /* the HKT constraint var name (`f`) */
    Type     functor_ty;                 /* the resolved concrete functor ctor */
    bool     have_functor_ty;
    uint64_t hash;
    /* VBM3: the abstract lens param's `Binding *` (the `l` of `set-px`), so the
     * poly-call emit can recognize `(l g s)` and redirect it. */
    const void *lensparam_binding;
    /* CM3: the lens param's positional slot in the consumer's arg list (the arg
     * a call site passes the concrete lens in), filled by the resolve pass; -1
     * until resolved.  CM3 uses it to elide the lens arg at the rewritten call. */
    int lens_idx;
    /* CM1: the resolved concrete-lens SET for this (consumer, lens-param).  The
     * VBM3 in-place redirect fires only when `n_lens_set == 1` (the unique case);
     * `n_lens_set >= 2` is the ambiguous case CM2/CM3 cover with consumer clones;
     * `n_lens_set == 0` is unresolved (runtime-selected lens -> Path A). */
    LensResolution lens_set[MONO_SPEC_LENSSET_MAX];
    size_t         n_lens_set;
    bool           lens_set_capped;
    /* CB5 backstop (was the CM4 partial-graduation gate).  A COMPOSED lens is
     * one whose body tails into another lens rather than into a direct `fmap`
     * dispatch -- see lens_is_simple_for_pathb.  CM4 poisoned EVERY composed
     * lens, because the outer by-value `g` straddled its carrier-lowered nested
     * lenses; CB1-CB5 closed that by threading `(f a)` by value through the
     * whole chain (docs/archive/history/van-laarhoven-composed-byvalue-plan.md),
     * so this flag is now set only for the residue CB2/CB3 still cannot lower:
     * a runtime-selected nested lens, a non-lens tail, a missing composition
     * adapter, or nesting past MONO_NESTED_DEPTH_MAX -- see
     * composed_lens_byvalueable.  A poisoned key falls FULLY back to Path A:
     * every Path-B accessor below reports "nothing" for it, so even simple
     * lenses sharing the same consumer stay on the carrier rather than risk a
     * mixed by-value/carrier miscompile. */
    bool           has_composed_lens;
} MonoSpecKey;

static MonoSpecKey g_specs[MONO_SPEC_MAX];
static size_t      g_n_specs = 0;

/* A CONCRETE spec key resolved by VBM2: the abstract lens param collapsed to the
 * concrete lens defn actually passed at a top-level invocation. */
typedef struct {
    char        lens[MONO_SPEC_FIELD];   /* concrete lens defn name (`point-x`) */
    char        functor[MONO_SPEC_FIELD];
    char        focus[MONO_SPEC_FIELD];
    char        whole[MONO_SPEC_FIELD];
    char        tyvar[MONO_SPEC_FIELD];
    Type        functor_ty;
    bool        have_functor_ty;
    const void *lens_fn;                 /* resolved `const FnDef *` for the lens */
    uint64_t    hash;
} MonoConcreteKey;

static MonoConcreteKey g_concrete[MONO_SPEC_MAX];
static size_t          g_n_concrete = 0;

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; s && *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

/* Copy `src` (default "?") into a fixed field, truncating to fit. */
static void field_copy(char *dst, const char *src) {
    if (!src || !*src) src = "?";
    size_t n = strlen(src);
    if (n >= MONO_SPEC_FIELD) n = MONO_SPEC_FIELD - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void mono_spec_register(const char *enclosing_fn, const char *callee,
                        const char *functor_name, const char *focus_ty,
                        const char *whole_ty, const char *tyvar,
                        const void *functor_ty, const void *lensparam_binding) {
    if (g_n_specs >= MONO_SPEC_MAX) return;
    MonoSpecKey k;
    memset(&k, 0, sizeof k);
    field_copy(k.enclosing, enclosing_fn);
    field_copy(k.callee, callee);
    field_copy(k.functor, functor_name);
    field_copy(k.focus, focus_ty);
    field_copy(k.whole, whole_ty);
    field_copy(k.tyvar, tyvar);
    k.lensparam_binding = lensparam_binding;
    k.lens_idx = -1;
    if (functor_ty) { k.functor_ty = *(const Type *)functor_ty; k.have_functor_ty = true; }
    /* Content hash over the five resolved fields.  Byte-identical keys collapse
     * (a lens invoked twice from the same enclosing fn registers once).  Two
     * sites in DIFFERENT enclosing fns that pin the same (functor, focus, whole)
     * still surface separately here because the concrete lens FnDef the plan's
     * canonical key names is not resolvable at the abstract `(l g s)` pin -- the
     * cross-procedural collapse to one CONCRETE emit is VBM2's resolve pass
     * (mono_specs_resolve_program) below. */
    uint64_t h = fnv1a(k.enclosing);
    h = h * 1099511628211ULL + fnv1a(k.callee);
    h = h * 1099511628211ULL + fnv1a(k.functor);
    h = h * 1099511628211ULL + fnv1a(k.focus);
    h = h * 1099511628211ULL + fnv1a(k.whole);
    k.hash = h;
    for (size_t i = 0; i < g_n_specs; i++)
        if (g_specs[i].hash == h) return; /* dedup */
    g_specs[g_n_specs++] = k;
}

static const MonoSpecKey *spec_for_binding(const void *lensparam_binding);

size_t mono_spec_count(void) { return g_n_specs; }
size_t mono_spec_concrete_count(void) { return g_n_concrete; }

const void *mono_spec_abstract_binding(size_t i) {
    return i < g_n_specs ? g_specs[i].lensparam_binding : NULL;
}

const char *mono_spec_abstract_enclosing(size_t i) {
    return i < g_n_specs ? g_specs[i].enclosing : NULL;
}

const void *mono_spec_consumer_call_lookup(const char *callee_name,
                                           int *lens_idx_out) {
    if (!callee_name) return NULL;
    for (size_t i = 0; i < g_n_specs; i++) {
        const MonoSpecKey *k = &g_specs[i];
        if (k->has_composed_lens) continue; /* poisoned -> Path A, never cloned */
        if (k->n_lens_set < 2) continue;   /* only ambiguous consumers are cloned */
        if (k->lens_idx < 0) continue;
        if (strcmp(k->enclosing, callee_name) != 0) continue;
        if (lens_idx_out) *lens_idx_out = k->lens_idx;
        return k->lensparam_binding;
    }
    return NULL;
}

bool mono_spec_consumer_is_generic(const void *lensparam_binding) {
    const MonoSpecKey *k = spec_for_binding(lensparam_binding);
    if (!k) return false;
    /* A generic consumer (`[S A]`-polymorphic) pins its lens focus/whole to type
     * variables; a monomorphic consumer pins concrete types.  `tyvar` in either
     * field is the reliable signal -- used to suppress the by-value redirect in
     * the consumer's GENERIC CARRIER base body (its concrete ABI-spec body still
     * redirects), since the base's lens-result consumer is the carrier form. */
    return strcmp(k->focus, "tyvar") == 0 || strcmp(k->whole, "tyvar") == 0;
}

unsigned long long mono_spec_lens_clone_hash(const void *lensparam_binding,
                                             const char *lens_name) {
    if (!lens_name) return 0;
    const MonoSpecKey *k = spec_for_binding(lensparam_binding);
    if (!k || k->has_composed_lens) return 0;
    for (size_t j = 0; j < k->n_lens_set; j++)
        if (strcmp(k->lens_set[j].lens_name, lens_name) == 0)
            return (unsigned long long)k->lens_set[j].mono_hash;
    return 0;
}

const char *mono_spec_abstract_tyvar(size_t i, const void **functor_ty) {
    if (i >= g_n_specs) { if (functor_ty) *functor_ty = NULL; return NULL; }
    const MonoSpecKey *k = &g_specs[i];
    if (functor_ty) *functor_ty = k->have_functor_ty ? &k->functor_ty : NULL;
    return k->tyvar;
}

unsigned long long mono_spec_concrete_get(size_t i, const char **lens,
                                          const char **functor,
                                          const char **focus,
                                          const char **whole) {
    if (i >= g_n_concrete) return 0;
    const MonoConcreteKey *k = &g_concrete[i];
    if (lens)    *lens    = k->lens;
    if (functor) *functor = k->functor;
    if (focus)   *focus   = k->focus;
    if (whole)   *whole   = k->whole;
    return (unsigned long long)k->hash;
}

unsigned long long mono_spec_concrete_emit_info(size_t i, const void **lens_fn,
                                                const char **tyvar,
                                                const void **functor_ty) {
    if (i >= g_n_concrete) return 0;
    const MonoConcreteKey *k = &g_concrete[i];
    if (lens_fn)    *lens_fn    = k->lens_fn;
    if (tyvar)      *tyvar      = k->tyvar;
    if (functor_ty) *functor_ty = k->have_functor_ty ? &k->functor_ty : NULL;
    return (unsigned long long)k->hash;
}

unsigned long long mono_spec_mono_hash_for_lens(const char *lens_name,
                                                const char *functor_name) {
    if (!lens_name) return 0;
    for (size_t i = 0; i < g_n_concrete; i++) {
        const MonoConcreteKey *k = &g_concrete[i];
        if (strcmp(k->lens, lens_name) != 0) continue;
        if (functor_name && *functor_name &&
            strcmp(k->functor, functor_name) != 0)
            continue;
        return (unsigned long long)k->hash;
    }
    return 0;
}

bool mono_spec_redirect_for_binding(const void *lensparam_binding,
                                    const char **lens_name,
                                    unsigned long long *mono_hash) {
    if (!lensparam_binding) return false;
    for (size_t i = 0; i < g_n_specs; i++) {
        const MonoSpecKey *k = &g_specs[i];
        if (k->lensparam_binding != lensparam_binding) continue;
        if (k->has_composed_lens) return false; /* poisoned -> Path A */
        /* CM1: the in-place VBM3 redirect is only safe when the param resolves to
         * exactly ONE concrete lens.  `== 0` (unresolved) and `>= 2` (ambiguous,
         * CM2/CM3's consumer-clone case) both fall through to Path A here. */
        if (k->n_lens_set != 1) return false;
        if (lens_name) *lens_name = k->lens_set[0].lens_name;
        if (mono_hash) *mono_hash = (unsigned long long)k->lens_set[0].mono_hash;
        return true;
    }
    return false;
}

/* CM1: the abstract spec whose lens param is `lensparam_binding` (NULL if none). */
static const MonoSpecKey *spec_for_binding(const void *lensparam_binding) {
    if (!lensparam_binding) return NULL;
    for (size_t i = 0; i < g_n_specs; i++)
        if (g_specs[i].lensparam_binding == lensparam_binding) return &g_specs[i];
    return NULL;
}

size_t mono_spec_lens_set_count(const void *lensparam_binding) {
    const MonoSpecKey *k = spec_for_binding(lensparam_binding);
    if (!k || k->has_composed_lens) return 0;   /* poisoned -> Path A */
    return k->n_lens_set;
}

bool mono_spec_lens_set_get(const void *lensparam_binding, size_t i,
                            const char **lens_name, const void **lens_fn,
                            unsigned long long *mono_hash) {
    const MonoSpecKey *k = spec_for_binding(lensparam_binding);
    if (!k || k->has_composed_lens || i >= k->n_lens_set) return false;
    const LensResolution *r = &k->lens_set[i];
    if (lens_name) *lens_name = r->lens_name;
    if (lens_fn)   *lens_fn   = r->lens_fn;
    if (mono_hash) *mono_hash = (unsigned long long)r->mono_hash;
    return true;
}

size_t mono_spec_lens_set_sites(const void *lensparam_binding, size_t i,
                                const void **sites_out, size_t sites_cap) {
    const MonoSpecKey *k = spec_for_binding(lensparam_binding);
    if (!k || i >= k->n_lens_set) return 0;
    const LensResolution *r = &k->lens_set[i];
    if (sites_out) {
        size_t n = r->n_sites < sites_cap ? r->n_sites : sites_cap;
        for (size_t j = 0; j < n; j++) sites_out[j] = r->sites[j];
    }
    return r->n_sites;
}

/* Register (dedup by content hash) one resolved CONCRETE spec key, carrying the
 * emit handles (resolved lens FnDef + functor ctor Type + HKT tyvar name) VBM2b
 * needs to emit the specialized by-value body. */
static uint64_t mono_concrete_register(const char *lens, const MonoSpecKey *abs,
                                       const void *lens_fn) {
    MonoConcreteKey k;
    memset(&k, 0, sizeof k);
    field_copy(k.lens, lens);
    field_copy(k.functor, abs->functor);
    field_copy(k.focus, abs->focus);
    field_copy(k.whole, abs->whole);
    field_copy(k.tyvar, abs->tyvar);
    k.functor_ty = abs->functor_ty;
    k.have_functor_ty = abs->have_functor_ty;
    k.lens_fn = lens_fn;
    uint64_t h = fnv1a(k.lens);
    h = h * 1099511628211ULL + fnv1a(k.functor);
    h = h * 1099511628211ULL + fnv1a(k.focus);
    h = h * 1099511628211ULL + fnv1a(k.whole);
    k.hash = h;
    for (size_t i = 0; i < g_n_concrete; i++)
        if (g_concrete[i].hash == h) return h; /* dedup */
    if (g_n_concrete < MONO_SPEC_MAX) g_concrete[g_n_concrete++] = k;
    return h;
}

/* ------------------------------------------------------------------ *
 * VBM2 resolution pass: join abstract lens specs to concrete lenses.  *
 * ------------------------------------------------------------------ */

/* Peel type-erasing / fat-conversion wrappers off a call argument and, if it
 * bottoms out at a named variable (a global lens defn passed by name), return
 * that name.  NULL for anonymous `(fn ...)` lenses and non-var args. */
static const char *arg_global_fn_name(const Expr *arg) {
    while (arg) {
        if (arg->kind == EX_ASCRIBE)          arg = arg->as.ascribe_.inner;
        else if (arg->kind == EX_FN_TO_FAT)   arg = arg->as.fn_to_fat_.inner;
        else if (arg->kind == EX_POLY_TO_FAT) arg = arg->as.poly_to_fat_.inner;
        else if (arg->kind == EX_POLY_WRAP)   arg = arg->as.poly_wrap_.inner;
        else break;
    }
    if (arg && arg->kind == EX_VAR && arg->as.var.binding &&
        arg->as.var.binding->name && arg->as.var.binding->name->name)
        return arg->as.var.binding->name->name;
    return NULL;
}

static const FnDef *find_fndef(const Expr *prog, const char *name);

/* Peel wrappers off a call arg and return its `Binding *` if it bottoms out at a
 * variable (a lens threaded through a param), else NULL. */
static const Binding *arg_var_binding(const Expr *arg) {
    while (arg) {
        if (arg->kind == EX_ASCRIBE)          arg = arg->as.ascribe_.inner;
        else if (arg->kind == EX_FN_TO_FAT)   arg = arg->as.fn_to_fat_.inner;
        else if (arg->kind == EX_POLY_TO_FAT) arg = arg->as.poly_to_fat_.inner;
        else if (arg->kind == EX_POLY_WRAP)   arg = arg->as.poly_wrap_.inner;
        else break;
    }
    if (arg && arg->kind == EX_VAR && arg->as.var.binding)
        return arg->as.var.binding;
    return NULL;
}

/* CM1: add concrete lens `(lens_name, lens_fn, mono_hash)` to `k`'s resolved set,
 * deduping the lens by FnDef identity (or name when the FnDef is unknown) and the
 * site by pointer.  Sets `*grew` when the set gains a new lens OR a new site --
 * the fixpoint monotonicity signal.  Cap hits emit a one-shot stderr note and
 * leave the over-cap element on Path A (never a silent truncation). */
static void lens_set_add(MonoSpecKey *k, const char *lens_name,
                         const void *lens_fn, uint64_t mono_hash,
                         const Expr *site, bool *grew) {
    LensResolution *slot = NULL;
    for (size_t i = 0; i < k->n_lens_set; i++) {
        LensResolution *r = &k->lens_set[i];
        if ((lens_fn && r->lens_fn == lens_fn) ||
            (!lens_fn && strcmp(r->lens_name, lens_name) == 0)) { slot = r; break; }
    }
    if (!slot) {
        if (k->n_lens_set >= MONO_SPEC_LENSSET_MAX) {
            if (!k->lens_set_capped) {
                k->lens_set_capped = true;
                fprintf(stderr,
                        "; note: consumer-mono lens-set cap (%d) hit for '%s' "
                        "lens-param '%s'; over-cap lenses stay on Path A\n",
                        MONO_SPEC_LENSSET_MAX, k->enclosing, k->callee);
            }
            return;
        }
        slot = &k->lens_set[k->n_lens_set++];
        field_copy(slot->lens_name, lens_name);
        slot->lens_fn = lens_fn;
        slot->mono_hash = mono_hash;
        slot->n_sites = 0;
        slot->sites_capped = false;
        if (grew) *grew = true;
    }
    if (!site) return;
    for (size_t i = 0; i < slot->n_sites; i++)
        if (slot->sites[i] == site) return; /* dedup site by pointer */
    if (slot->n_sites >= MONO_SPEC_SITES_MAX) {
        if (!slot->sites_capped) {
            slot->sites_capped = true;
            fprintf(stderr,
                    "; note: consumer-mono site cap (%d) hit for '%s' lens '%s'\n",
                    MONO_SPEC_SITES_MAX, k->enclosing, slot->lens_name);
        }
        return;
    }
    slot->sites[slot->n_sites++] = site;
    if (grew) *grew = true;
}

/* CM4 partial-graduation: a lens is "simple" (by-value Path B eligible) iff its
 * body tail is a direct functor-method (`fmap`) dispatch -- it builds `(f A)` in
 * one step through the caller's `Functor` dict (e.g. `point-x`'s
 * `(fmap (g (.x s)) ...)`).  A COMPOSED lens (e.g. `line-a-x`, whose body is
 * `(line-a (fn [p] (point-x g p)) s)`) instead tails into a call to ANOTHER lens
 * with no dict dispatch: the nested lens is carrier-lowered while the outer `g`
 * is by value, and the two ABIs collide in the by-value mono body (CM4 gap 2).
 * Such lenses stay on Path A.  Peel ascribe/return/let/do wrappers to the tail. */
static bool lens_is_simple_for_pathb(const FnDef *lens) {
    if (!lens || !lens->body) return false;
    const Expr *e = lens->body;
    for (;;) {
        if (!e) return false;
        switch (e->kind) {
            case EX_REINTERPRET: e = e->as.reinterpret_.expr; continue;
            case EX_ASCRIBE: e = e->as.ascribe_.inner; continue;
            case EX_RETURN:  e = e->as.return_.value;  continue;
            case EX_LET:
            case EX_LETREC:  e = e->as.let_.body;      continue;
            case EX_DO:
                if (e->as.do_.n == 0) return false;
                e = e->as.do_.items[e->as.do_.n - 1];
                continue;
            default: break;
        }
        break;
    }
    /* Tail must be an `fmap` typeclass-method dispatch: an EX_CALL carrying an
     * EX_DICT `dict_arg` whose method is `fmap`. */
    if (e->kind != EX_CALL) return false;
    const Expr *da = e->as.call_.dict_arg;
    if (!da || da->kind != EX_DICT) return false;
    if (strcmp(da->as.dict_.method_name, "fmap") != 0) return false;
    /* wide-copy-struct-functor-show-dict: Path B monomorphizes ONLY the functor
     * tyvar `f`; it cannot resolve any OTHER constraint the lens carries (e.g. a
     * `^Show a` its mapper dispatches on the still-abstract element tyvar `a`).
     * Such a dispatch is lowered to a runtime dict the mapper env expects as a
     * dict-clone parameter -- which the by-value mono body has no slot for, so
     * emitting it references an undeclared dict binding and `cc` fails.  A van
     * Laarhoven lens always carries `^Functor f`; MORE than one constraint means
     * an extra dict Path B cannot thread, so defer such lenses to Path A (the
     * dict-clone, which threads every constraint dict by value/carrier and is
     * correct, just not zero-overhead for the wide functor). */
    if (lens->constraints.n_constraints > 1) return false;
    return true;
}

/* CB1 (van-laarhoven-composed-byvalue-plan): peel a type to its head constructor
 * and, if the head is a type variable, return its (interned) name; NULL else. */
static const char *ty_head_tyvar_name(const Type *t) {
    while (t && t->kind == TY_APP && t->as.app.fn) t = t->as.app.fn;
    if (t && t->kind == TY_TYVAR) return t->as.tyvar_.name;
    return NULL;
}

/* CB1: structural test that `fn` is a van Laarhoven lens -- it returns `(f S)`
 * (a type application headed by an HKT tyvar `f`) and carries a function-typed
 * parameter `g : (-> A (f A))` whose result is headed by the SAME `f`.  This is
 * the shape `point-x` / `line-a` / `line-a-x` share; ordinary helpers do not
 * match, so it discriminates a nested-lens application from any other global
 * call in a composed lens body. */
static bool fn_is_lens(const FnDef *fn) {
    if (!fn || !fn->body || fn->n_params == 0 || !fn->param_types) return false;
    if (fn->return_type.kind != TY_APP) return false;
    const char *fv = ty_head_tyvar_name(&fn->return_type);
    if (!fv) return false;
    for (uint32_t p = 0; p < fn->n_params; p++) {
        const Type *pt = &fn->param_types[p];
        if (pt->kind != TY_FN) continue;
        const Type *res = pt->as.fn.result_full_type;
        if (!res || res->kind != TY_APP) continue;
        const char *rv = ty_head_tyvar_name(res);
        if (rv && strcmp(rv, fv) == 0) return true;
    }
    return false;
}

/* CB1: a nested lens is applied through its MB1 *dict-clone* (`point-x__dict_N`,
 * `dict_clone_class` set) inside a composed lens body -- the clone shares the
 * ORIGINAL lens FnDef's `body`.  Map a dict-clone back to its original (the
 * non-clone top-level defn sharing that body) so we register / redirect the
 * original's by-value `<lens>__mono` body (whose `fmap` mints the by-value twin),
 * not the clone's (which dispatches through its runtime dict param). */
static const FnDef *resolve_orig_lens(const Expr *prog, const FnDef *fn) {
    if (!fn || fn->n_dict_clone == 0) return fn;   /* already the original */
    if (prog && prog->kind == EX_PROGRAM && fn->body) {
        for (uint32_t i = 0; i < prog->as.program.n; i++) {
            const Expr *it = prog->as.program.items[i];
            if (it && it->kind == EX_FN_DEF && it->as.fn_def_.fn &&
                it->as.fn_def_.fn->n_dict_clone == 0 &&
                it->as.fn_def_.fn->body == fn->body)
                return it->as.fn_def_.fn;
        }
    }
    return fn;   /* fallback: no original found (should not happen) */
}

/* CB1: recursively register the concrete keys for the nested lenses APPLIED in a
 * COMPOSED lens body, so their by-value `<lens>__mono` bodies get emitted (VBM2b)
 * and CB2's nested redirect can target them.  `abs` supplies the shared functor
 * pin; `prog` resolves global lens names to FnDefs.  `seen`/`n_seen` dedup lens
 * FnDefs and, together with `depth`, bound the walk against self-reference / deep
 * nesting (CB5) -- a nested lens that is itself composed contributes its own
 * nested lenses down to the depth cap. */
#define MONO_NESTED_SEEN_MAX 32
#define MONO_NESTED_DEPTH_MAX 8
static void register_nested_lenses(const Expr *e, const MonoSpecKey *abs,
                                   const Expr *prog, const FnDef **seen,
                                   size_t *n_seen, int depth);

static void nested_register_one(const FnDef *nlens, const MonoSpecKey *abs,
                                const Expr *prog, const FnDef **seen,
                                size_t *n_seen, int depth) {
    if (!nlens || depth > MONO_NESTED_DEPTH_MAX) return;
    for (size_t i = 0; i < *n_seen; i++) if (seen[i] == nlens) return;
    if (*n_seen >= MONO_NESTED_SEEN_MAX) return;
    seen[(*n_seen)++] = nlens;
    const char *nm = (nlens->binding && nlens->binding->name)
        ? nlens->binding->name->name : NULL;
    if (!nm) return;
    mono_concrete_register(nm, abs, nlens);
    if (!lens_is_simple_for_pathb(nlens) && nlens->body)
        register_nested_lenses(nlens->body, abs, prog, seen, n_seen, depth + 1);
}

static void register_nested_lenses(const Expr *e, const MonoSpecKey *abs,
                                   const Expr *prog, const FnDef **seen,
                                   size_t *n_seen, int depth) {
    if (!e) return;
    if (e->kind == EX_CALL) {
        const Binding *fb = e->as.call_.fn_binding;
        if (fb && fb->name && fb->name->name) {
            const FnDef *nlens = resolve_orig_lens(prog,
                                                   find_fndef(prog, fb->name->name));
            if (nlens && fn_is_lens(nlens))
                nested_register_one(nlens, abs, prog, seen, n_seen, depth);
        }
    }
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                register_nested_lenses(e->as.program.items[i], abs, prog,
                                       seen, n_seen, depth);
            break;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn)
                register_nested_lenses(e->as.fn_def_.fn->body, abs, prog,
                                       seen, n_seen, depth);
            break;
        case EX_FN:
            if (e->as.fn_.fn)
                register_nested_lenses(e->as.fn_.fn->body, abs, prog,
                                       seen, n_seen, depth);
            break;
        case EX_CLOSURE:
            if (e->as.closure_.closure && e->as.closure_.closure->fn)
                register_nested_lenses(e->as.closure_.closure->fn->body, abs,
                                       prog, seen, n_seen, depth);
            break;
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                register_nested_lenses(e->as.let_.bindings[i].init, abs, prog,
                                       seen, n_seen, depth);
            register_nested_lenses(e->as.let_.body, abs, prog, seen, n_seen, depth);
            break;
        case EX_IF:
            register_nested_lenses(e->as.if_.cond, abs, prog, seen, n_seen, depth);
            register_nested_lenses(e->as.if_.then_, abs, prog, seen, n_seen, depth);
            register_nested_lenses(e->as.if_.else_or_null, abs, prog, seen,
                                   n_seen, depth);
            break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                register_nested_lenses(e->as.do_.items[i], abs, prog, seen,
                                       n_seen, depth);
            break;
        case EX_WHILE:
            register_nested_lenses(e->as.while_.cond, abs, prog, seen, n_seen, depth);
            register_nested_lenses(e->as.while_.body, abs, prog, seen, n_seen, depth);
            break;
        case EX_SET:  register_nested_lenses(e->as.set_.value, abs, prog, seen, n_seen, depth); break;
        case EX_DEF:  register_nested_lenses(e->as.def_.init, abs, prog, seen, n_seen, depth); break;
        case EX_CALL:
            register_nested_lenses(e->as.call_.fn_expr, abs, prog, seen, n_seen, depth);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                register_nested_lenses(e->as.call_.args[i], abs, prog, seen,
                                       n_seen, depth);
            break;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                register_nested_lenses(e->as.builtin.args[i], abs, prog, seen,
                                       n_seen, depth);
            break;
        case EX_RETURN:  register_nested_lenses(e->as.return_.value, abs, prog, seen, n_seen, depth); break;
        case EX_ASCRIBE: register_nested_lenses(e->as.ascribe_.inner, abs, prog, seen, n_seen, depth); break;
        case EX_FN_TO_FAT:   register_nested_lenses(e->as.fn_to_fat_.inner, abs, prog, seen, n_seen, depth); break;
        case EX_POLY_TO_FAT: register_nested_lenses(e->as.poly_to_fat_.inner, abs, prog, seen, n_seen, depth); break;
        case EX_BYVAL_CLONE: register_nested_lenses(e->as.byval_clone_.expr, abs, prog, seen, n_seen, depth); break;
        case EX_POLY_WRAP:   register_nested_lenses(e->as.poly_wrap_.inner, abs, prog, seen, n_seen, depth); break;
        case EX_MATCH:
            register_nested_lenses(e->as.match_.scrutinee, abs, prog, seen, n_seen, depth);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                register_nested_lenses(e->as.match_.arms[i].body, abs, prog, seen, n_seen, depth);
                register_nested_lenses(e->as.match_.arms[i].guard, abs, prog, seen, n_seen, depth);
            }
            break;
        default:
            break;
    }
}

/* CB5 (van-laarhoven-composed-byvalue-plan): peel `e` to its tail expression
 * (through ascribe/return/let/do wrappers). */
static const Expr *peel_to_tail(const Expr *e) {
    for (;;) {
        if (!e) return NULL;
        switch (e->kind) {
            case EX_REINTERPRET: e = e->as.reinterpret_.expr; continue;
            case EX_ASCRIBE: e = e->as.ascribe_.inner; continue;
            case EX_RETURN:  e = e->as.return_.value;  continue;
            case EX_LET:
            case EX_LETREC:  e = e->as.let_.body;      continue;
            case EX_DO:
                if (e->as.do_.n == 0) return NULL;
                e = e->as.do_.items[e->as.do_.n - 1];
                continue;
            default: return e;
        }
    }
}

/* CB5: a COMPOSED lens is Path-B eligible only when it matches the shape CB2/CB3
 * lower by value: its body tails into `(<nested-lens> adapter s)` (a lens call,
 * NOT an `fmap` dispatch) whose callee resolves to a lens FnDef, AND it carries
 * an adapter closure `(fn [p] ...)` whose own tail resolves to a lens FnDef
 * (recursively, for depth > 2).  Anything else -- a runtime-selected nested lens,
 * a non-lens tail, a missing adapter -- has no by-value lowering and MUST stay on
 * the Path A carrier (the `has_composed_lens` backstop), so it is never admitted
 * here.  Bounded by `depth` against self-reference / deep nesting. */
static bool composed_lens_byvalueable(const FnDef *lens, const Expr *prog,
                                      int depth) {
    if (!lens || !lens->body || depth > MONO_NESTED_DEPTH_MAX) return false;
    const Expr *tail = peel_to_tail(lens->body);
    if (!tail || tail->kind != EX_CALL) return false;
    /* A simple lens (fmap dispatch) is handled elsewhere; not composed. */
    if (tail->as.call_.dict_arg && tail->as.call_.dict_arg->kind == EX_DICT)
        return false;
    /* Tail callee must resolve to a lens FnDef. */
    const Binding *tfb = tail->as.call_.fn_binding;
    if (!tfb || !tfb->name || !tfb->name->name) return false;
    const FnDef *tlens = resolve_orig_lens(prog, find_fndef(prog, tfb->name->name));
    if (!tlens || !fn_is_lens(tlens)) return false;
    if (!lens_is_simple_for_pathb(tlens) &&
        !composed_lens_byvalueable(tlens, prog, depth + 1))
        return false;
    /* Must carry an adapter closure whose own tail resolves to a lens FnDef. */
    for (uint32_t i = 0; i < tail->as.call_.n_args; i++) {
        const Expr *a = tail->as.call_.args[i];
        while (a) {
            if (a->kind == EX_ASCRIBE)          a = a->as.ascribe_.inner;
            else if (a->kind == EX_FN_TO_FAT)   a = a->as.fn_to_fat_.inner;
            else if (a->kind == EX_POLY_TO_FAT) a = a->as.poly_to_fat_.inner;
            else if (a->kind == EX_POLY_WRAP)   a = a->as.poly_wrap_.inner;
            else break;
        }
        const FnDef *afn = NULL;
        if (a && a->kind == EX_CLOSURE && a->as.closure_.closure)
            afn = a->as.closure_.closure->fn;
        else if (a && a->kind == EX_FN)
            afn = a->as.fn_.fn;
        if (!afn || !afn->body) continue;
        const Expr *atail = peel_to_tail(afn->body);
        if (!atail || atail->kind != EX_CALL) continue;
        const Binding *afb = atail->as.call_.fn_binding;
        if (!afb || !afb->name || !afb->name->name) continue;
        const FnDef *anl = resolve_orig_lens(prog,
                                             find_fndef(prog, afb->name->name));
        if (anl && fn_is_lens(anl) &&
            (lens_is_simple_for_pathb(anl) ||
             composed_lens_byvalueable(anl, prog, depth + 1)))
            return true;   /* found a valid composition adapter */
    }
    return false;
}

typedef struct {
    const char  *enclosing; /* enclosing fn name whose calls we resolve */
    uint32_t     lens_idx;  /* arg slot holding the concrete lens */
    MonoSpecKey *abs;       /* the abstract key being resolved (mutable) */
    const Expr  *prog;      /* program root, for lens-name -> FnDef */
    bool        *grew;      /* set true when abs's lens set grows (fixpoint) */
} ResolveCtx;

/* Recursively visit `e`, growing `rc->abs`'s concrete-lens set for every EX_CALL
 * of `rc->enclosing`.  A lens-slot arg that names a global lens defn adds that
 * concrete lens directly; an arg that forwards ANOTHER consumer's (already
 * resolved) lens param inherits that param's whole set (the transitive step the
 * fixpoint drives to closure). */
static void resolve_walk(const Expr *e, const ResolveCtx *rc) {
    if (!e) return;
    if (e->kind == EX_CALL) {
        const Binding *fb = e->as.call_.fn_binding;
        if (fb && fb->name && fb->name->name &&
            strcmp(fb->name->name, rc->enclosing) == 0 &&
            rc->lens_idx < e->as.call_.n_args) {
            const Expr *larg = e->as.call_.args[rc->lens_idx];
            const char *lens = arg_global_fn_name(larg);
            const FnDef *lens_fn = lens ? find_fndef(rc->prog, lens) : NULL;
            if (lens && lens_fn) {
                /* CB1 (van-laarhoven-composed-byvalue-plan): register the concrete
                 * lens (simple OR composed) and add it to the consumer's set.  A
                 * COMPOSED lens additionally registers the concrete keys of the
                 * nested lenses applied in its body, so their by-value
                 * `<lens>__mono` bodies are emitted (VBM2b) and CB2's nested
                 * redirect can target them -- the whole composed chain threads
                 * `(f a)` by value with no carrier box (superseding the CM4
                 * `has_composed_lens` Path A gate). */
                bool simple = lens_is_simple_for_pathb(lens_fn);
                if (!simple &&
                    !composed_lens_byvalueable(lens_fn, rc->prog, 1)) {
                    /* CB5 backstop: a composed lens whose shape CB2/CB3 cannot
                     * lower by value (runtime-selected nested lens, non-lens tail,
                     * missing adapter, depth past the cap) stays on Path A -- do
                     * NOT register a concrete key (which would emit an ill-typed
                     * `<lens>__mono`) and poison the consumer so even simple lenses
                     * sharing it stay on the carrier. */
                    rc->abs->has_composed_lens = true;
                } else {
                    uint64_t h = mono_concrete_register(lens, rc->abs, lens_fn);
                    lens_set_add(rc->abs, lens, lens_fn, h, e, rc->grew);
                    if (!simple && lens_fn->body) {
                        const FnDef *seen[MONO_NESTED_SEEN_MAX];
                        size_t n_seen = 0;
                        register_nested_lenses(lens_fn->body, rc->abs, rc->prog,
                                               seen, &n_seen, 1);
                    }
                }
            } else {
                /* Transitive: the arg is a variable.  If it is a consumer lens
                 * param whose own set is already (partly) resolved, fold that set
                 * in, keying each element to THIS call site (the one CM3 rewrites
                 * inside the forwarding consumer's clone).  Grows monotonically to
                 * a fixpoint since the lens universe is finite. */
                const Binding *vb = arg_var_binding(larg);
                const MonoSpecKey *src = spec_for_binding(vb);
                if (src) {
                    /* Poison propagates transitively: forwarding a poisoned
                     * consumer's lens param keeps the receiver on Path A too. */
                    if (src->has_composed_lens) rc->abs->has_composed_lens = true;
                    for (size_t j = 0; j < src->n_lens_set; j++) {
                        const LensResolution *r = &src->lens_set[j];
                        uint64_t h = mono_concrete_register(r->lens_name, rc->abs,
                                                            r->lens_fn);
                        lens_set_add(rc->abs, r->lens_name, r->lens_fn, h, e,
                                     rc->grew);
                    }
                }
            }
        }
    }
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                resolve_walk(e->as.program.items[i], rc);
            break;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn) resolve_walk(e->as.fn_def_.fn->body, rc);
            break;
        case EX_FN:
            if (e->as.fn_.fn) resolve_walk(e->as.fn_.fn->body, rc);
            break;
        case EX_CLOSURE:
            if (e->as.closure_.closure && e->as.closure_.closure->fn)
                resolve_walk(e->as.closure_.closure->fn->body, rc);
            break;
        case EX_LET:
        case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                resolve_walk(e->as.let_.bindings[i].init, rc);
            resolve_walk(e->as.let_.body, rc);
            break;
        case EX_IF:
            resolve_walk(e->as.if_.cond, rc);
            resolve_walk(e->as.if_.then_, rc);
            resolve_walk(e->as.if_.else_or_null, rc);
            break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                resolve_walk(e->as.do_.items[i], rc);
            break;
        case EX_WHILE:
            resolve_walk(e->as.while_.cond, rc);
            resolve_walk(e->as.while_.body, rc);
            break;
        case EX_SET:
            resolve_walk(e->as.set_.value, rc);
            break;
        case EX_DEF:
            resolve_walk(e->as.def_.init, rc);
            break;
        case EX_CALL:
            resolve_walk(e->as.call_.fn_expr, rc);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                resolve_walk(e->as.call_.args[i], rc);
            break;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                resolve_walk(e->as.builtin.args[i], rc);
            break;
        case EX_RETURN:
            resolve_walk(e->as.return_.value, rc);
            break;
        case EX_REINTERPRET:
            resolve_walk(e->as.reinterpret_.expr, rc);
            break;
        case EX_ASCRIBE:
            resolve_walk(e->as.ascribe_.inner, rc);
            break;
        case EX_FN_TO_FAT:
            resolve_walk(e->as.fn_to_fat_.inner, rc);
            break;
        case EX_POLY_TO_FAT:
            resolve_walk(e->as.poly_to_fat_.inner, rc);
            break;
        case EX_BYVAL_CLONE:
            resolve_walk(e->as.byval_clone_.expr, rc);
            break;
        case EX_POLY_WRAP:
            resolve_walk(e->as.poly_wrap_.inner, rc);
            break;
        case EX_MATCH:
            resolve_walk(e->as.match_.scrutinee, rc);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                resolve_walk(e->as.match_.arms[i].body, rc);
                resolve_walk(e->as.match_.arms[i].guard, rc);
            }
            break;
        default:
            break;
    }
}

/* Find the top-level FnDef named `name` in the elaborated program. */
static const FnDef *find_fndef(const Expr *prog, const char *name) {
    if (!prog || prog->kind != EX_PROGRAM || !name) return NULL;
    for (uint32_t i = 0; i < prog->as.program.n; i++) {
        const Expr *it = prog->as.program.items[i];
        if (it && it->kind == EX_FN_DEF && it->as.fn_def_.fn &&
            it->as.fn_def_.fn->binding && it->as.fn_def_.fn->binding->name &&
            it->as.fn_def_.fn->binding->name->name &&
            strcmp(it->as.fn_def_.fn->binding->name->name, name) == 0)
            return it->as.fn_def_.fn;
    }
    return NULL;
}

/* VBM4-residual: peel wrappers off a call arg and return its closure FnDef (the
 * `g : (-> A (f A))` closure), or NULL. */
static FnDef *arg_closure_fndef(const Expr *arg) {
    while (arg) {
        if (arg->kind == EX_ASCRIBE)          arg = arg->as.ascribe_.inner;
        else if (arg->kind == EX_FN_TO_FAT)   arg = arg->as.fn_to_fat_.inner;
        else if (arg->kind == EX_POLY_TO_FAT) arg = arg->as.poly_to_fat_.inner;
        else if (arg->kind == EX_POLY_WRAP)   arg = arg->as.poly_wrap_.inner;
        else break;
    }
    if (!arg) return NULL;
    if (arg->kind == EX_CLOSURE && arg->as.closure_.closure)
        return arg->as.closure_.closure->fn;
    if (arg->kind == EX_FN) return arg->as.fn_.fn;
    if (arg->kind == EX_VAR && arg->as.var.binding)
        return arg->as.var.binding->source_fn_def;
    return NULL;
}

/* VBM4-residual: find the `(l g s)` call in a redirected consumer's body (`l` ==
 * `lensb`) and clear `box_aggregate_result` on the `g` closure, so it returns its
 * `(f A)` aggregate BY VALUE instead of the int64 carrier box.  Safe only because
 * the consumer is redirected to `point_x__mono` (which now takes a by-value `g`);
 * a NON-redirected consumer keeps its boxed `g` for the Path A carrier dispatch. */
static void clear_g_box_walk(const Expr *e, const Binding *lensb) {
    if (!e) return;
    if (e->kind == EX_CALL && e->as.call_.fn_binding == lensb) {
        for (uint32_t i = 0; i < e->as.call_.n_args; i++) {
            FnDef *g = arg_closure_fndef(e->as.call_.args[i]);
            if (g && g->box_aggregate_result) g->box_aggregate_result = false;
        }
    }
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                clear_g_box_walk(e->as.program.items[i], lensb);
            break;
        case EX_FN_DEF: if (e->as.fn_def_.fn) clear_g_box_walk(e->as.fn_def_.fn->body, lensb); break;
        case EX_FN: if (e->as.fn_.fn) clear_g_box_walk(e->as.fn_.fn->body, lensb); break;
        case EX_CLOSURE:
            if (e->as.closure_.closure && e->as.closure_.closure->fn)
                clear_g_box_walk(e->as.closure_.closure->fn->body, lensb);
            break;
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                clear_g_box_walk(e->as.let_.bindings[i].init, lensb);
            clear_g_box_walk(e->as.let_.body, lensb);
            break;
        case EX_IF:
            clear_g_box_walk(e->as.if_.cond, lensb);
            clear_g_box_walk(e->as.if_.then_, lensb);
            clear_g_box_walk(e->as.if_.else_or_null, lensb);
            break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                clear_g_box_walk(e->as.do_.items[i], lensb);
            break;
        case EX_CALL:
            clear_g_box_walk(e->as.call_.fn_expr, lensb);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                clear_g_box_walk(e->as.call_.args[i], lensb);
            break;
        case EX_RETURN: clear_g_box_walk(e->as.return_.value, lensb); break;
        case EX_REINTERPRET: clear_g_box_walk(e->as.reinterpret_.expr, lensb); break;
        case EX_ASCRIBE: clear_g_box_walk(e->as.ascribe_.inner, lensb); break;
        case EX_MATCH:
            clear_g_box_walk(e->as.match_.scrutinee, lensb);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++)
                clear_g_box_walk(e->as.match_.arms[i].body, lensb);
            break;
        default: break;
    }
}

/* CM3-transitive: fill `lens_idx` (the lens param's positional slot) for every
 * spec whose enclosing fn is resolvable and slot is not yet known. */
static void compute_lens_indices(const Expr *prog) {
    for (size_t i = 0; i < g_n_specs; i++) {
        MonoSpecKey *k = &g_specs[i];
        if (k->lens_idx >= 0) continue;
        const FnDef *enc = find_fndef(prog, k->enclosing);
        if (!enc) continue;
        for (uint32_t p = 0; p < enc->n_params; p++) {
            const Binding *pb = enc->params ? enc->params[p] : NULL;
            if (pb && pb->name && pb->name->name &&
                strcmp(pb->name->name, k->callee) == 0) {
                k->lens_idx = p;
                break;
            }
        }
    }
}

/* CM3-transitive: is `b` a positional parameter binding of `fd`? */
static bool is_param_of(const FnDef *fd, const Binding *b) {
    if (!fd || !b || !fd->params) return false;
    for (uint32_t p = 0; p < fd->n_params; p++)
        if ((const Binding *)fd->params[p] == b) return true;
    return false;
}

typedef struct {
    const FnDef *enc;   /* nearest lexically-enclosing fn (NULL at top level) */
    bool        *grew;  /* set when a new forwarding spec is registered */
} FwdCtx;

/* CM3-transitive: register a mono spec for a FORWARDING lens param.  When a call
 * `(C p ...)` inside fn `E` passes `E`'s own parameter `p` into consumer `C`'s
 * lens slot, `p` is a lens threaded one level deeper.  Register an abstract spec
 * `(E, p)` that INHERITS `C`'s functor/focus/whole (the consumer fixes those), so
 * the CM1 fixpoint can resolve `p` from `E`'s own call sites and `C` then inherits
 * `p`'s set transitively.  Dedups by content hash in `mono_spec_register`. */
static void register_forwarding_walk(const Expr *e, FwdCtx *fc) {
    if (!e) return;
    const FnDef *save = fc->enc;
    if (e->kind == EX_FN_DEF && e->as.fn_def_.fn) fc->enc = e->as.fn_def_.fn;
    else if (e->kind == EX_FN && e->as.fn_.fn) fc->enc = e->as.fn_.fn;
    else if (e->kind == EX_CLOSURE && e->as.closure_.closure &&
             e->as.closure_.closure->fn)
        fc->enc = e->as.closure_.closure->fn;
    if (e->kind == EX_CALL && fc->enc) {
        const Binding *fb = e->as.call_.fn_binding;
        if (fb && fb->name && fb->name->name) {
            for (size_t i = 0; i < g_n_specs; i++) {
                const MonoSpecKey *C = &g_specs[i];
                if (C->lens_idx < 0) continue;   /* slot not yet known */
                if (strcmp(C->enclosing, fb->name->name) != 0) continue;
                if ((uint32_t)C->lens_idx >= e->as.call_.n_args) continue;
                const Binding *vb =
                    arg_var_binding(e->as.call_.args[C->lens_idx]);
                if (!vb || !is_param_of(fc->enc, vb)) continue;
                const char *ename = (fc->enc->binding && fc->enc->binding->name)
                    ? fc->enc->binding->name->name : NULL;
                const char *pname = vb->name ? vb->name->name : NULL;
                if (!ename || !pname) continue;
                size_t before = g_n_specs;
                mono_spec_register(ename, pname, C->functor, C->focus, C->whole,
                                   C->tyvar,
                                   C->have_functor_ty ? &C->functor_ty : NULL,
                                   vb);
                if (g_n_specs != before && fc->grew) *fc->grew = true;
            }
        }
    }
    switch (e->kind) {
        case EX_PROGRAM:
            for (uint32_t i = 0; i < e->as.program.n; i++)
                register_forwarding_walk(e->as.program.items[i], fc);
            break;
        case EX_FN_DEF:
            if (e->as.fn_def_.fn) register_forwarding_walk(e->as.fn_def_.fn->body, fc);
            break;
        case EX_FN:
            if (e->as.fn_.fn) register_forwarding_walk(e->as.fn_.fn->body, fc);
            break;
        case EX_CLOSURE:
            if (e->as.closure_.closure && e->as.closure_.closure->fn)
                register_forwarding_walk(e->as.closure_.closure->fn->body, fc);
            break;
        case EX_LET: case EX_LETREC:
            for (uint32_t i = 0; i < e->as.let_.n; i++)
                register_forwarding_walk(e->as.let_.bindings[i].init, fc);
            register_forwarding_walk(e->as.let_.body, fc);
            break;
        case EX_IF:
            register_forwarding_walk(e->as.if_.cond, fc);
            register_forwarding_walk(e->as.if_.then_, fc);
            register_forwarding_walk(e->as.if_.else_or_null, fc);
            break;
        case EX_DO:
            for (uint32_t i = 0; i < e->as.do_.n; i++)
                register_forwarding_walk(e->as.do_.items[i], fc);
            break;
        case EX_WHILE:
            register_forwarding_walk(e->as.while_.cond, fc);
            register_forwarding_walk(e->as.while_.body, fc);
            break;
        case EX_SET:  register_forwarding_walk(e->as.set_.value, fc); break;
        case EX_DEF:  register_forwarding_walk(e->as.def_.init, fc); break;
        case EX_CALL:
            register_forwarding_walk(e->as.call_.fn_expr, fc);
            for (uint32_t i = 0; i < e->as.call_.n_args; i++)
                register_forwarding_walk(e->as.call_.args[i], fc);
            break;
        case EX_BUILTIN:
            for (uint32_t i = 0; i < e->as.builtin.n; i++)
                register_forwarding_walk(e->as.builtin.args[i], fc);
            break;
        case EX_RETURN:  register_forwarding_walk(e->as.return_.value, fc); break;
        case EX_REINTERPRET: register_forwarding_walk(e->as.reinterpret_.expr, fc); break;
        case EX_ASCRIBE: register_forwarding_walk(e->as.ascribe_.inner, fc); break;
        case EX_FN_TO_FAT:   register_forwarding_walk(e->as.fn_to_fat_.inner, fc); break;
        case EX_POLY_TO_FAT: register_forwarding_walk(e->as.poly_to_fat_.inner, fc); break;
        case EX_BYVAL_CLONE: register_forwarding_walk(e->as.byval_clone_.expr, fc); break;
        case EX_POLY_WRAP:   register_forwarding_walk(e->as.poly_wrap_.inner, fc); break;
        case EX_MATCH:
            register_forwarding_walk(e->as.match_.scrutinee, fc);
            for (uint32_t i = 0; i < e->as.match_.n_arms; i++) {
                register_forwarding_walk(e->as.match_.arms[i].body, fc);
                register_forwarding_walk(e->as.match_.arms[i].guard, fc);
            }
            break;
        default: break;
    }
    fc->enc = save;
}

void mono_specs_resolve_program(const void *prog_) {
    const Expr *prog = (const Expr *)prog_;
    if (!prog || prog->kind != EX_PROGRAM || g_n_specs == 0) return;
    /* CM3-transitive: before resolving, register a spec for every forwarding lens
     * param (a consumer that threads its param into another consumer's lens slot)
     * so the fixpoint below can resolve it.  Iterate to a fixpoint -- a param may
     * forward through several levels -- computing slots for freshly-added specs
     * each round.  Monotonic: specs only accrue, bounded by the finite set of
     * (fn, param) pairs. */
    compute_lens_indices(prog);
    for (size_t fpass = 0; ; fpass++) {
        bool grew = false;
        FwdCtx fc = { NULL, &grew };
        register_forwarding_walk(prog, &fc);
        if (grew) compute_lens_indices(prog);
        if (!grew) break;
        if (fpass + 1 >= MONO_SPEC_FIXPOINT_PASSES) {
            fprintf(stderr,
                    "; note: consumer-mono forwarding-spec fixpoint hit the "
                    "%d-pass cap; some forwarded lenses stay on Path A\n",
                    MONO_SPEC_FIXPOINT_PASSES);
            break;
        }
    }
    /* CM1 fixpoint: each pass grows every consumer lens param's concrete-lens set
     * (directly from named-lens call args, transitively from forwarded params);
     * repeat until a pass adds nothing.  Terminates because sets only grow toward
     * the finite universe of concrete lens FnDefs.  The pass cap is a backstop --
     * if it ever trips, something is adding non-monotonically; note it loudly. */
    size_t pass = 0;
    for (;;) {
        bool grew = false;
        for (size_t i = 0; i < g_n_specs; i++) {
            MonoSpecKey *k = &g_specs[i];
            /* The abstract lens param lives on the enclosing fn; find its slot. */
            const FnDef *enc = find_fndef(prog, k->enclosing);
            if (!enc) continue;
            int lens_idx = -1;
            for (uint32_t p = 0; p < enc->n_params; p++) {
                const Binding *pb = enc->params ? enc->params[p] : NULL;
                if (pb && pb->name && pb->name->name &&
                    strcmp(pb->name->name, k->callee) == 0) {
                    lens_idx = p;
                    break;
                }
            }
            if (lens_idx < 0) continue;
            k->lens_idx = lens_idx;   /* CM3: remember the lens arg slot */
            ResolveCtx rc = { k->enclosing, (uint32_t)lens_idx, k, prog, &grew };
            resolve_walk(prog, &rc);
        }
        if (!grew) break;
        if (++pass >= MONO_SPEC_FIXPOINT_PASSES) {
            fprintf(stderr,
                    "; note: consumer-mono resolve fixpoint hit the %d-pass cap; "
                    "lens sets may be incomplete (over-cap uses stay on Path A)\n",
                    MONO_SPEC_FIXPOINT_PASSES);
            break;
        }
    }
    /* VBM4-residual: a consumer whose lens param resolves UNIQUELY (|set| == 1)
     * has its `(l g s)` redirected to the by-value `<lens>__mono`.  Clear the `g`
     * closure's box flag so it returns `(f A)` by value -- removing the last small
     * heap box.  The ambiguous case (|set| >= 2) keeps its boxed `g` here; CM2
     * de-boxes each consumer clone independently. */
    for (size_t i = 0; i < g_n_specs; i++) {
        MonoSpecKey *k = &g_specs[i];
        if (k->n_lens_set != 1 || !k->lensparam_binding) continue;
        const FnDef *enc = find_fndef(prog, k->enclosing);
        if (enc && enc->params && enc->body)
            clear_g_box_walk(enc->body, (const Binding *)k->lensparam_binding);
    }
}

void mono_specs_dump(FILE *out) {
    if (!out) return;
    fprintf(out,
            "; van-laarhoven by-value monomorphization specs: %zu abstract, "
            "%zu concrete\n",
            g_n_specs, g_n_concrete);
    for (size_t i = 0; i < g_n_specs; i++) {
        const MonoSpecKey *k = &g_specs[i];
        fprintf(out,
                "mono-spec-abstract %016llx fn=%s lens-param=%s f=%s focus=%s "
                "whole=%s\n",
                (unsigned long long)k->hash, k->enclosing, k->callee,
                k->functor, k->focus, k->whole);
    }
    for (size_t i = 0; i < g_n_concrete; i++) {
        const MonoConcreteKey *k = &g_concrete[i];
        fprintf(out,
                "mono-spec %016llx lens=%s f=%s focus=%s whole=%s\n",
                (unsigned long long)k->hash, k->lens, k->functor, k->focus,
                k->whole);
    }
    /* CM1: the resolved concrete-lens SET per consumer lens param, so the
     * fixpoint is reviewable by eye.  A `{}` set is an unresolved (runtime-
     * selected) lens; a singleton is the VBM3 unique-redirect case; `>= 2` is the
     * ambiguous case CM2/CM3 specialize with consumer clones. */
    for (size_t i = 0; i < g_n_specs; i++) {
        const MonoSpecKey *k = &g_specs[i];
        fprintf(out, "mono-spec-set fn=%s lens-param=%s -> {", k->enclosing,
                k->callee);
        for (size_t j = 0; j < k->n_lens_set; j++)
            fprintf(out, "%s%s", j ? ", " : "", k->lens_set[j].lens_name);
        fprintf(out, "}%s\n", k->lens_set_capped ? " (capped)" : "");
    }
}

void mono_specs_reset(void) { g_n_specs = 0; g_n_concrete = 0; }
