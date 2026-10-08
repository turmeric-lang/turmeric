#include "effect.h"

#include <string.h>
#include "buf.h"

/* Create an empty effect row */
EffectRow *effect_row_empty(Arena *a) {
    EffectRow *row = arena_alloc(a, sizeof(EffectRow));
    row->kind = ERK_EMPTY;
    return row;
}

/* Create a concrete effect row with a single effect */
EffectRow *effect_row_single(Arena *a, Effect *effect) {
    EffectRow *row = arena_alloc(a, sizeof(EffectRow));
    row->kind = ERK_CONCRETE;
    row->as.concrete.effects = arena_alloc(a, sizeof(Effect *));
    row->as.concrete.effects[0] = effect;
    row->as.concrete.n_effects = 1;
    return row;
}

/* Create a concrete effect row from multiple effects */
EffectRow *effect_row_concrete(Arena *a, Effect **effects, uint8_t n_effects) {
    EffectRow *row = arena_alloc(a, sizeof(EffectRow));
    row->kind = ERK_CONCRETE;
    row->as.concrete.effects = arena_alloc(a, n_effects * sizeof(Effect *));
    memcpy(row->as.concrete.effects, effects, n_effects * sizeof(Effect *));
    row->as.concrete.n_effects = n_effects;
    return row;
}

/* Create an effect row union */
EffectRow *effect_row_union(Arena *a, EffectRow *left, EffectRow *right) {
    EffectRow *row = arena_alloc(a, sizeof(EffectRow));
    row->kind = ERK_UNION;
    row->as.union_.left = left;
    row->as.union_.right = right;
    return row;
}

/* Create an unresolved (symbolic) effect row from an array of symbol names. */
EffectRow *effect_row_unresolved(Arena *a, const Symbol **sym_names,
                                 uint8_t n_sym_names, Span span) {
    if (n_sym_names == 0) return effect_row_empty(a);
    EffectRow *row = arena_alloc(a, sizeof(EffectRow));
    row->kind = ERK_UNRESOLVED;
    row->as.unresolved.sym_names = arena_alloc(a, n_sym_names * sizeof(Symbol *));
    for (uint8_t i = 0; i < n_sym_names; i++) {
        row->as.unresolved.sym_names[i] = sym_names[i];
    }
    row->as.unresolved.n_sym_names = n_sym_names;
    row->as.unresolved.span = span;
    row->as.unresolved.unknown_reported = false;
    return row;
}

/* An uppercase-initial name in a row is an effect; anything else is a row
 * variable.  effect_row_resolve and effect_row_unknown_names must agree on
 * this, or a name could be dropped without being reported. */
static bool row_name_is_effect(const Symbol *name) {
    return !(name->name[0] >= 'a' && name->name[0] <= 'z');
}

uint8_t effect_row_unknown_names(const EffectRow *row, EffectEnv *env,
                                 const Symbol **out, uint8_t cap) {
    if (!row || row->kind != ERK_UNRESOLVED) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < row->as.unresolved.n_sym_names; i++) {
        const Symbol *name = row->as.unresolved.sym_names[i];
        if (!name || name->len == 0 || !row_name_is_effect(name)) continue;
        if (effect_env_lookup(env, name)) continue;
        if (n < cap) out[n] = name;
        n++;
    }
    return n;
}

/* Resolve ERK_UNRESOLVED → concrete/var using the populated effect environment. */
EffectRow *effect_row_resolve(EffectRow *row, EffectEnv *env, Arena *a) {
    if (!row || row->kind != ERK_UNRESOLVED) return row;
    if (row->as.unresolved.n_sym_names == 0) return effect_row_empty(a);

    EffectRow *result = effect_row_empty(a);
    for (uint8_t i = 0; i < row->as.unresolved.n_sym_names; i++) {
        const Symbol *name = row->as.unresolved.sym_names[i];
        if (!name || name->len == 0) continue;
        /* Lowercase first character → row variable; uppercase → concrete effect. */
        if (!row_name_is_effect(name)) {
            EffectRow *var = arena_alloc(a, sizeof(EffectRow));
            var->kind = ERK_VAR;
            var->as.var.var_name = name;
            result = effect_row_merge(a, result, var);
        } else {
            Effect *eff = effect_env_lookup(env, name);
            if (eff) {
                result = effect_row_merge(a, result, effect_row_single(a, eff));
            }
            /* Unknown uppercase name: left out.  The caller has already
             * reported it (TUR-E0026, effect_check.c resolve_declared_row);
             * there is nothing to resolve it TO. */
        }
    }
    return result;
}

/* ET4: Returns true if `child` is the same as `parent_eff` or is a descendant via ^extends */
bool effect_is_subeffect(const Effect *child, const Effect *parent_eff) {
    if (!child || !parent_eff) return child == parent_eff;
    const Effect *e = child;
    while (e) {
        if (e == parent_eff) return true;
        e = e->parent;
    }
    return false;
}

/* Check if an effect row is empty */
bool effect_row_is_empty(EffectRow *row) {
    if (!row) return true;
    return row->kind == ERK_EMPTY;
}

bool effect_row_is_runtime_pure(const EffectRow *row) {
    if (!row) return true;
    switch (row->kind) {
    case ERK_EMPTY:
        return true;
    case ERK_CONCRETE:
        for (uint8_t i = 0; i < row->as.concrete.n_effects; i++) {
            const Effect *eff = row->as.concrete.effects[i];
            if (!eff || !eff->is_capability) return false;
        }
        return true;
    case ERK_UNION:
        return effect_row_is_runtime_pure(row->as.union_.left) &&
               effect_row_is_runtime_pure(row->as.union_.right);
    case ERK_VAR:
    case ERK_UNRESOLVED:
        return false;
    }
    return false;
}

EffectRow *effect_row_without_capabilities(Arena *a, EffectRow *row) {
    if (!row) return row;
    switch (row->kind) {
    case ERK_CONCRETE: {
        uint8_t n = row->as.concrete.n_effects, kept = 0;
        for (uint8_t i = 0; i < n; i++)
            if (row->as.concrete.effects[i] && !row->as.concrete.effects[i]->is_capability)
                kept++;
        if (kept == n) return row;
        if (kept == 0) return effect_row_empty(a);
        Effect **keep = arena_alloc(a, kept * sizeof(Effect *));
        uint8_t k = 0;
        for (uint8_t i = 0; i < n; i++)
            if (row->as.concrete.effects[i] && !row->as.concrete.effects[i]->is_capability)
                keep[k++] = row->as.concrete.effects[i];
        return effect_row_concrete(a, keep, kept);
    }
    case ERK_UNION: {
        EffectRow *l = effect_row_without_capabilities(a, row->as.union_.left);
        EffectRow *r = effect_row_without_capabilities(a, row->as.union_.right);
        if (l == row->as.union_.left && r == row->as.union_.right) return row;
        return effect_row_merge(a, l, r);
    }
    default:
        return row;
    }
}

/* Helper to compare two effects by pointer (they're arena-allocated) */
static bool effect_ptr_eq(Effect *a, Effect *b) {
    return a == b;
}

/* Check if an effect row contains a specific effect */
bool effect_row_contains(EffectRow *row, Effect *effect) {
    if (!row) return false;
    
    switch (row->kind) {
        case ERK_EMPTY:
            return false;
        case ERK_CONCRETE: {
            for (uint8_t i = 0; i < row->as.concrete.n_effects; i++) {
                if (effect_ptr_eq(row->as.concrete.effects[i], effect)) {
                    return true;
                }
                /* ET4: Check if row effect is a parent (ancestor) of eff */
                if (effect_is_subeffect(effect, row->as.concrete.effects[i])) {
                    return true;
                }
            }
            return false;
        }
        case ERK_VAR:
            /* Effect variables are polymorphic - we can't determine at this level */
            return false;
        case ERK_UNION:
            return effect_row_contains(row->as.union_.left, effect) ||
                   effect_row_contains(row->as.union_.right, effect);
        case ERK_UNRESOLVED:
            /* Unresolved row — conservatively say no. */
            return false;
    }
    return false;
}

/* Check if effect row r1 is a subset of r2 (r1 ⊆ r2) */
bool effect_row_is_subset(EffectRow *r1, EffectRow *r2) {
    if (!r1) return true;  /* Empty is subset of everything */
    if (!r2) return effect_row_is_empty(r1);
    
    if (r1->kind == ERK_EMPTY) return true;
    if (r2->kind == ERK_EMPTY) return false;
    
    if (r1->kind == ERK_VAR || r2->kind == ERK_VAR) {
        /* For now, we treat effect variables as unknown - can't determine subset */
        return true; /* Conservative: allow it */
    }
    
    if (r1->kind == ERK_CONCRETE && r2->kind == ERK_CONCRETE) {
        /* Check each effect in r1 is in r2 */
        for (uint8_t i = 0; i < r1->as.concrete.n_effects; i++) {
            Effect *e = r1->as.concrete.effects[i];
            if (!effect_row_contains(r2, e)) {
                return false;
            }
        }
        return true;
    }
    
    if (r1->kind == ERK_UNION) {
        return effect_row_is_subset(r1->as.union_.left, r2) &&
               effect_row_is_subset(r1->as.union_.right, r2);
    }
    
    /* Other cases: conservative */
    return true;
}

/* Check if two concrete effect rows are equal (same effects, order-independent) */
static bool effect_row_concrete_equal(EffectRow *r1, EffectRow *r2) {
    if (r1->as.concrete.n_effects != r2->as.concrete.n_effects) return false;
    
    for (uint8_t i = 0; i < r1->as.concrete.n_effects; i++) {
        Effect *e = r1->as.concrete.effects[i];
        bool found = false;
        for (uint8_t j = 0; j < r2->as.concrete.n_effects; j++) {
            if (effect_ptr_eq(e, r2->as.concrete.effects[j])) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

/* Create a union of two effect rows - merges them into a single concrete row */
EffectRow *effect_row_merge(Arena *a, EffectRow *r1, EffectRow *r2) {
    if (effect_row_is_empty(r1)) return r2;
    if (effect_row_is_empty(r2)) return r1;
    
    if (r1->kind == ERK_EMPTY) return r2;
    if (r2->kind == ERK_EMPTY) return r1;
    
    /* For now, handle the simple case: both are concrete */
    if (r1->kind == ERK_CONCRETE && r2->kind == ERK_CONCRETE) {
        /* Check if they're the same */
        if (effect_row_concrete_equal(r1, r2)) {
            return r1; /* or r2, they're equal */
        }
        
        /* Merge: collect all unique effects from both */
        Effect *all_effects[32]; /* Max effects in a row for now */
        uint8_t n_all = 0;
        
        /* Add from r1 */
        for (uint8_t i = 0; i < r1->as.concrete.n_effects; i++) {
            bool already = false;
            for (uint8_t j = 0; j < n_all; j++) {
                if (effect_ptr_eq(all_effects[j], r1->as.concrete.effects[i])) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                all_effects[n_all++] = r1->as.concrete.effects[i];
            }
        }
        
        /* Add from r2 */
        for (uint8_t i = 0; i < r2->as.concrete.n_effects; i++) {
            bool already = false;
            for (uint8_t j = 0; j < n_all; j++) {
                if (effect_ptr_eq(all_effects[j], r2->as.concrete.effects[i])) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                all_effects[n_all++] = r2->as.concrete.effects[i];
            }
        }
        
        return effect_row_concrete(a, all_effects, n_all);
    }
    
    /* For unions and variables, just create a union node */
    return effect_row_union(a, r1, r2);
}

/* Return a copy of `row` with the named effect removed (effect re-opening).
 * Only ERK_CONCRETE rows are modified; other kinds are returned unchanged. */
EffectRow *effect_row_remove(EffectRow *row, const Symbol *effect_name, Arena *a) {
    if (!row || row->kind != ERK_CONCRETE) return row;
    Effect *keep[32];
    uint8_t n_keep = 0;
    for (uint8_t i = 0; i < row->as.concrete.n_effects; i++) {
        Effect *eff = row->as.concrete.effects[i];
        if (eff->name != effect_name) {
            keep[n_keep++] = eff;
        }
    }
    if (n_keep == 0) return effect_row_empty(a);
    return effect_row_concrete(a, keep, n_keep);
}

/* Print an effect row for debugging */
void effect_row_print(Buf *b, EffectRow *row) {
    if (!row) {
        buf_puts(b, "<null>");
        return;
    }
    
    switch (row->kind) {
        case ERK_EMPTY:
            buf_puts(b, "{}");
            break;
        case ERK_CONCRETE:
            buf_puts(b, "{");
            for (uint8_t i = 0; i < row->as.concrete.n_effects; i++) {
                if (i > 0) buf_puts(b, ", ");
                buf_printf(b, "%s", row->as.concrete.effects[i]->name->name);
            }
            buf_puts(b, "}");
            break;
        case ERK_VAR:
            buf_printf(b, "{%s}", row->as.var.var_name->name);
            break;
        case ERK_UNION:
            buf_puts(b, "(");
            effect_row_print(b, row->as.union_.left);
            buf_puts(b, " | ");
            effect_row_print(b, row->as.union_.right);
            buf_puts(b, ")");
            break;
        case ERK_UNRESOLVED:
            buf_puts(b, "#{");
            for (uint8_t i = 0; i < row->as.unresolved.n_sym_names; i++) {
                if (i > 0) buf_puts(b, " ");
                buf_printf(b, "%s", row->as.unresolved.sym_names[i]->name);
            }
            buf_puts(b, "}");
            break;
    }
}

/* FH4.1: collect the effect *names* of a row into `out` (deduplicated), used to
 * compare/print handler handled-sets without needing resolved Effect* pointers.
 * Handles UNRESOLVED (symbolic names), CONCRETE (resolved effects), VAR, and
 * UNION (recursively).  `cap` bounds the output array. */
void effect_row_collect_names(EffectRow *row, const Symbol **out,
                              uint8_t *n, uint8_t cap) {
    if (!row || !out || !n) return;
    switch (row->kind) {
        case ERK_EMPTY:
            break;
        case ERK_UNRESOLVED:
            for (uint8_t i = 0; i < row->as.unresolved.n_sym_names; i++) {
                const Symbol *s = row->as.unresolved.sym_names[i];
                bool dup = false;
                for (uint8_t j = 0; j < *n; j++) if (out[j] == s) { dup = true; break; }
                if (!dup && *n < cap) out[(*n)++] = s;
            }
            break;
        case ERK_CONCRETE:
            for (uint8_t i = 0; i < row->as.concrete.n_effects; i++) {
                const Symbol *s = row->as.concrete.effects[i]->name;
                bool dup = false;
                for (uint8_t j = 0; j < *n; j++) if (out[j] == s) { dup = true; break; }
                if (!dup && *n < cap) out[(*n)++] = s;
            }
            break;
        case ERK_VAR: {
            const Symbol *s = row->as.var.var_name;
            bool dup = false;
            for (uint8_t j = 0; j < *n; j++) if (out[j] == s) { dup = true; break; }
            if (!dup && *n < cap) out[(*n)++] = s;
            break;
        }
        case ERK_UNION:
            effect_row_collect_names(row->as.union_.left, out, n, cap);
            effect_row_collect_names(row->as.union_.right, out, n, cap);
            break;
    }
}

/* FH4.1: true if two rows denote the same *set* of effect names (order-
 * insensitive).  NULL rows compare equal to each other and to empty rows. */
bool effect_row_name_set_eq(EffectRow *a, EffectRow *b) {
    const Symbol *an[32]; uint8_t na = 0;
    const Symbol *bn[32]; uint8_t nb = 0;
    effect_row_collect_names(a, an, &na, 32);
    effect_row_collect_names(b, bn, &nb, 32);
    if (na != nb) return false;
    for (uint8_t i = 0; i < na; i++) {
        bool found = false;
        for (uint8_t j = 0; j < nb; j++) if (an[i] == bn[j]) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

/* FH4.1: append a row's effect names to `b`, separated by " | " (no braces);
 * used to render handler<...> type names. */
void effect_row_format_names(Buf *b, EffectRow *row) {
    const Symbol *names[32]; uint8_t n = 0;
    effect_row_collect_names(row, names, &n, 32);
    if (n == 0) { buf_puts(b, "?"); return; }
    for (uint8_t i = 0; i < n; i++) {
        if (i > 0) buf_puts(b, " | ");
        buf_puts(b, names[i]->name);
    }
}

/* Effect environment - global registry of effects */
EffectEnv *effect_env_new(Arena *a) {
    EffectEnv *env = arena_alloc(a, sizeof(EffectEnv));
    env->effects = NULL;
    env->n_effects = 0;
    env->cap_effects = 0;
    return env;
}

/* Register a new effect in the environment */
Effect *effect_env_register(EffectEnv *env, Arena *a, const Symbol *name,
                            const Symbol **param_names, TypeKind *param_types,
                            uint8_t n_params, TypeKind result_type,
                            const Symbol *defining_module_name, bool is_private) {
    /* Check if already exists */
    for (uint32_t i = 0; i < env->n_effects; i++) {
        if (env->effects[i]->name == name) {
            return env->effects[i]; /* Already registered */
        }
    }

    /* Allocate space if needed */
    if (env->n_effects >= env->cap_effects) {
        uint32_t new_cap = env->cap_effects ? env->cap_effects * 2 : 8;
        Effect **new_array = arena_alloc(a, new_cap * sizeof(Effect *));
        if (env->effects) {
            memcpy(new_array, env->effects, env->n_effects * sizeof(Effect *));
        }
        env->effects = new_array;
        env->cap_effects = new_cap;
    }

    /* Create the effect */
    Effect *effect = arena_alloc(a, sizeof(Effect));
    effect->name = name;
    effect->is_polymorphic = false; /* For now, all effects are monomorphic */
    effect->defining_module_name = defining_module_name;
    effect->is_private = is_private;
    effect->is_exported = !is_private;
    effect->parent = NULL;  /* ET4: no parent by default */
    effect->is_capability = false;  /* stdlib-effect-rows: set later for ^capability effects */

    /* Create the constructor (1:1 for now) */
    EffectConstructor *ctor = arena_alloc(a, sizeof(EffectConstructor));
    ctor->name = name;
    ctor->param_names = param_names;
    ctor->param_types = param_types;
    ctor->n_params = n_params;
    ctor->result_type = result_type;
    ctor->effect = effect;
    ctor->result_full_type = NULL;   /* Tier C: set by defeffect when aggregate */
    ctor->param_full_types = NULL;
    ctor->resumable_payload_param = -1;   /* set by defeffect from the param form */

    effect->constructor = ctor;

    env->effects[env->n_effects++] = effect;
    return effect;
}

/* Look up an effect by name */
void effect_redefine(Effect *eff, const Symbol **param_names,
                     TypeKind *param_types, uint8_t n_params,
                     TypeKind result_type,
                     const Symbol *defining_module_name, bool is_private) {
    if (!eff) return;
    eff->defining_module_name = defining_module_name;
    eff->is_private           = is_private;
    eff->is_exported          = !is_private;
    eff->parent               = NULL;
    eff->is_capability        = false;
    EffectConstructor *ctor = eff->constructor;
    if (!ctor) return;
    ctor->param_names             = param_names;
    ctor->param_types             = param_types;
    ctor->n_params                = n_params;
    ctor->result_type             = result_type;
    ctor->result_full_type        = NULL;
    ctor->param_full_types        = NULL;
    ctor->resumable_payload_param = -1;
}

Effect *effect_env_lookup(EffectEnv *env, const Symbol *name) {
    for (uint32_t i = 0; i < env->n_effects; i++) {
        if (env->effects[i]->name == name) {
            return env->effects[i];
        }
    }
    return NULL;
}

/* Check if an effect is valid (exists in environment) */
bool effect_env_contains(EffectEnv *env, const Symbol *name) {
    return effect_env_lookup(env, name) != NULL;
}

Effect *effect_env_register_builtin_unsafe(EffectEnv *env, Arena *a,
                                           const Symbol *unsafe_name) {
    if (!env || !a || !unsafe_name) return NULL;
    return effect_env_register(env, a, unsafe_name,
                               NULL, NULL, 0, TY_NIL, NULL, false);
}

/* effect-row-honesty-plan W2: the capability tags the stdlib's I/O modules
 * are annotated with.  `IO` is the umbrella; the rest `^extends IO`.  They
 * were declared only in stdlib/effects.tur, which is not autoloaded, so
 * `#fx{IO}` meant nothing (and, after W1, was TUR-E0026) in exactly the small
 * programs most likely to write it.  Order matters: a parent precedes its
 * children. */
static const struct { const char *name; const char *parent; } k_builtin_caps[] = {
    { "IO",   NULL },
    { "FS",   "IO" },
    { "Net",  "IO" },
    { "Proc", "IO" },
    { "Rand", "IO" },
};

bool effect_builtin_capability(const char *name, const char **parent_out) {
    if (!name) return false;
    for (size_t i = 0; i < sizeof(k_builtin_caps) / sizeof(k_builtin_caps[0]); i++) {
        if (strcmp(name, k_builtin_caps[i].name) == 0) {
            if (parent_out) *parent_out = k_builtin_caps[i].parent;
            return true;
        }
    }
    return false;
}

void effect_env_register_builtin_capabilities(EffectEnv *env, Arena *a,
                                              SymbolTable *st) {
    if (!env || !a || !st) return;
    for (size_t i = 0; i < sizeof(k_builtin_caps) / sizeof(k_builtin_caps[0]); i++) {
        const char *nm = k_builtin_caps[i].name;
        const Symbol *sym = symtab_intern(st, strslice(nm, (uint32_t)strlen(nm)));
        Effect *eff = effect_env_register(env, a, sym, NULL, NULL, 0, TY_NIL,
                                          NULL, false);
        if (!eff) continue;
        eff->is_capability = true;
        if (k_builtin_caps[i].parent) {
            const char *pn = k_builtin_caps[i].parent;
            eff->parent = effect_env_lookup(
                env, symtab_intern(st, strslice(pn, (uint32_t)strlen(pn))));
        }
    }
}

/* ---------------------------------------------------------------------------
 * Phase P19-4: EffectRowSubst — effect-row variable unification
 * --------------------------------------------------------------------------- */

EffectRowSubst *effect_row_subst_new(Arena *a) {
    EffectRowSubst *s = arena_alloc(a, sizeof(EffectRowSubst));
    s->n = 0;
    return s;
}

void effect_row_subst_bind(EffectRowSubst *subst,
                            const Symbol *var, EffectRow *row) {
    if (!subst || !var || !row) return;
    /* First-wins: do not overwrite an existing binding. */
    for (uint32_t i = 0; i < subst->n; i++) {
        if (subst->entries[i].var == var) return;
    }
    if (subst->n >= EFFECT_ROW_SUBST_CAP) return; /* silently drop when full */
    subst->entries[subst->n].var = var;
    subst->entries[subst->n].row = row;
    subst->n++;
}

EffectRow *effect_row_subst_lookup(const EffectRowSubst *subst,
                                    const Symbol *var) {
    if (!subst || !var) return NULL;
    for (uint32_t i = 0; i < subst->n; i++) {
        if (subst->entries[i].var == var)
            return subst->entries[i].row;
    }
    return NULL;
}

bool effect_row_unify(EffectRow *r1, EffectRow *r2,
                      EffectRowSubst *subst, Arena *a) {
    if (!r1 || !r2) return true; /* NULL rows unify trivially */

    /* Simplify: apply existing substitutions first. */
    if (r1->kind == ERK_VAR) {
        /* Trivial case: same variable on both sides, no binding needed. */
        if (r2->kind == ERK_VAR && r1->as.var.var_name == r2->as.var.var_name)
            return true;
        EffectRow *bound = effect_row_subst_lookup(subst, r1->as.var.var_name);
        if (bound) return effect_row_unify(bound, r2, subst, a);
        if (effect_row_occurs(r1->as.var.var_name, r2))
            return false; /* occurs check: binding would produce an infinite row */
        effect_row_subst_bind(subst, r1->as.var.var_name, r2);
        return true;
    }
    if (r2->kind == ERK_VAR) {
        /* Trivial case: same variable on both sides, no binding needed. */
        if (r1->kind == ERK_VAR && r2->as.var.var_name == r1->as.var.var_name)
            return true;
        EffectRow *bound = effect_row_subst_lookup(subst, r2->as.var.var_name);
        if (bound) return effect_row_unify(r1, bound, subst, a);
        if (effect_row_occurs(r2->as.var.var_name, r1))
            return false; /* occurs check: binding would produce an infinite row */
        effect_row_subst_bind(subst, r2->as.var.var_name, r1);
        return true;
    }

    /* Both concrete: succeed (we allow concrete rows to be supersets; exact
     * subset checking is done separately in effect_row_check_declared). */
    if (r1->kind == ERK_EMPTY && r2->kind == ERK_EMPTY) return true;
    if (r1->kind == ERK_EMPTY || r2->kind == ERK_EMPTY) {
        /* One empty, one non-empty: succeed — the non-empty row just means
         * the callee uses those effects at the call site. */
        return true;
    }
    /* Both concrete (or union): succeed permissively in v1. */
    (void)a;
    return true;
}

bool effect_row_occurs(const Symbol *var_name, EffectRow *row) {
    if (!row) return false;
    switch (row->kind) {
    case ERK_EMPTY:
        return false;
    case ERK_VAR:
        return var_name == row->as.var.var_name; /* pointer equality: symbols are interned */
    case ERK_CONCRETE:
        return false; /* concrete rows have no variables */
    case ERK_UNION:
        return effect_row_occurs(var_name, row->as.union_.left) ||
               effect_row_occurs(var_name, row->as.union_.right);
    case ERK_UNRESOLVED:
        return false; /* already resolved by this point */
    }
    return false; /* unreachable */
}

EffectRow *effect_row_apply_subst(EffectRow *row,
                                   const EffectRowSubst *subst, Arena *a) {
    if (!row) return NULL;
    switch (row->kind) {
    case ERK_EMPTY:
    case ERK_CONCRETE:
        return row; /* No variables — return unchanged. */

    case ERK_VAR: {
        EffectRow *bound = effect_row_subst_lookup(subst, row->as.var.var_name);
        if (bound) {
            /* Recursively apply substitution to the bound value. */
            return effect_row_apply_subst(bound, subst, a);
        }
        return row; /* Unbound — keep as-is. */
    }

    case ERK_UNION: {
        EffectRow *left  = effect_row_apply_subst(row->as.union_.left,  subst, a);
        EffectRow *right = effect_row_apply_subst(row->as.union_.right, subst, a);
        return effect_row_union(a, left, right);
    }

    case ERK_UNRESOLVED:
        return row; /* Should be resolved before substitution; return as-is. */
    }
    return row; /* unreachable */
}
