#include "typeclass.h"
#include "forms.h"     /* class-superclasses: the unresolved `(Super var)` forms */
#include "prof.h"      /* --timings: instance resolution is "typeclass" */

#include <stdlib.h>
#include <string.h>



/* Initialize typeclass environment */
void typeclass_env_init(TypeClassEnv *env, Arena *arena) {
    env->arena = arena;
    env->typeclasses = NULL;
    env->instances = NULL;
}

/* Register a typeclass */
TypeClass *typeclass_env_register_typeclass(TypeClassEnv *env, const Symbol *name) {
    /* Check if already registered */
    TypeClass *existing = typeclass_env_lookup_typeclass(env, name);
    if (existing) {
        return existing;
    }
    
    TypeClass *tc = (TypeClass *)arena_alloc(env->arena, sizeof(TypeClass));
    if (!tc) return NULL;
    
    tc->name = name;
    tc->type_params = NULL;
    tc->n_type_params = 0;
    tc->methods = NULL;
    tc->n_methods = 0;
    tc->assoc_type_names = NULL;
    tc->n_assoc_types = 0;
    tc->has_fundep = false;
    tc->fundep_from_mask = 0;
    tc->fundep_to_mask = 0;
    tc->super_forms = NULL;
    tc->super_n_args = NULL;
    tc->super_arg_idx = NULL;
    tc->n_supers = 0;
    tc->supers = NULL;
    tc->decl_form = NULL;
    tc->from_stdlib = false;
    tc->next = env->typeclasses;
    env->typeclasses = tc;
    
    return tc;
}

/* Register a typeclass instance */
TypeClassInstance *typeclass_env_register_instance(TypeClassEnv *env, TypeClass *typeclass) {
    TypeClassInstance *inst = (TypeClassInstance *)arena_alloc(env->arena, sizeof(TypeClassInstance));
    if (!inst) return NULL;
    
    inst->typeclass = typeclass;
    inst->type_args = NULL;
    inst->type_arg_syms = NULL;
    inst->n_type_args = 0;
    inst->method_impls = NULL;
    inst->n_method_impls = 0;
    inst->dyn_witness = NULL;        /* D8 Q3: filled on demand by a dispatch site */
    inst->constraints = NULL;
    inst->n_constraints = 0;
    /* Phase PTC1: Type parameter constraints */
    inst->type_param_constraints = NULL;
    inst->n_type_param_constraints = 0;
    inst->decl_form = NULL;
    inst->super_obligations_ok = false;
    inst->assoc_types = NULL;
    inst->n_assoc_types = 0;
    inst->origin_file_id = 0;
    inst->partial_hole_pos = 0xFF;
    inst->dyn_any_minted = false;
    inst->next = env->instances;
    env->instances = inst;
    
    return inst;
}

/* Look up a typeclass by name */
const TypeClassMethod *typeclass_env_find_method(const TypeClassEnv *env,
                                                 const Symbol *name,
                                                 const TypeClass **out_class) {
    if (!env || !name) return NULL;
    for (TypeClass *tc = env->typeclasses; tc != NULL; tc = tc->next) {
        for (uint8_t i = 0; i < tc->n_methods; i++) {
            if (tc->methods[i].name == name) {
                if (out_class) *out_class = tc;
                return &tc->methods[i];
            }
        }
    }
    return NULL;
}

TypeClass *typeclass_env_lookup_typeclass(const TypeClassEnv *env, const Symbol *name) {
    for (TypeClass *tc = env->typeclasses; tc != NULL; tc = tc->next) {
        if (tc->name == name) {
            return tc;
        }
    }
    return NULL;
}

/* ---- class-superclasses (docs/archive/typeclass-superclasses-plan.md) ---- */

/* Same class by name: a class re-registered through two import paths is two
 * TypeClass records with one interned Symbol, and the dispatch path has always
 * counted that as the same class (elab_method_call's constraint walk). */
static bool tc_same_name(const TypeClass *a, const TypeClass *b) {
    if (a == b) return true;
    if (!a || !b || !a->name || !b->name) return false;
    return a->name == b->name ||
           (a->name->len == b->name->len &&
            memcmp(a->name->name, b->name->name, a->name->len) == 0);
}

/* The class the i-th superclass element of `tc` names: the resolved slot when
 * the post-unit pass has run, else a by-name lookup of the form's head. */
static TypeClass *tc_super_at(const TypeClassEnv *env, const TypeClass *tc, uint8_t i) {
    if (tc->supers && tc->supers[i]) return tc->supers[i];
    if (!tc->super_forms || !tc->super_forms[i]) return NULL;
    const Form *f = tc->super_forms[i];
    if (f->tag != F_LIST || f->as.list.len < 1 || f->as.list.items[0]->tag != F_SYM)
        return NULL;
    return typeclass_env_lookup_typeclass(env, f->as.list.items[0]->as.sym);
}

bool typeclass_resolve_superclasses(const TypeClassEnv *env, TypeClass *tc) {
    if (!tc || tc->n_supers == 0) return true;
    if (!tc->supers) {
        tc->supers = (TypeClass **)arena_alloc(env->arena,
                                              tc->n_supers * sizeof(TypeClass *));
        if (!tc->supers) return false;
        for (uint8_t i = 0; i < tc->n_supers; i++) tc->supers[i] = NULL;
    }
    bool ok = true;
    for (uint8_t i = 0; i < tc->n_supers; i++) {
        if (tc->supers[i]) continue;
        tc->supers[i] = tc_super_at(env, tc, i);
        if (!tc->supers[i]) ok = false;
    }
    return ok;
}

/* Bounded DFS over the superclass graph.  The class count in any real program
 * is tiny (the whole stdlib registers a few dozen), so a fixed visited array
 * with a linear membership test is the right size of machinery; a walk that
 * would exceed it stops rather than recursing without bound. */
#define TC_WALK_MAX 256

bool typeclass_entails(const TypeClassEnv *env, TypeClass *sub, const TypeClass *sup) {
    if (!sub || !sup) return false;
    if (tc_same_name(sub, sup)) return true;
    TypeClass *stack[TC_WALK_MAX];
    TypeClass *seen[TC_WALK_MAX];
    uint32_t n_stack = 0, n_seen = 0;
    stack[n_stack++] = sub;
    seen[n_seen++]   = sub;
    while (n_stack > 0) {
        TypeClass *cur = stack[--n_stack];
        for (uint8_t i = 0; i < cur->n_supers; i++) {
            TypeClass *s = tc_super_at(env, cur, i);
            if (!s) continue;                 /* not declared (yet) */
            if (tc_same_name(s, sup)) return true;
            bool dup = false;
            for (uint32_t k = 0; k < n_seen && !dup; k++) dup = (seen[k] == s);
            if (dup) continue;
            if (n_seen >= TC_WALK_MAX || n_stack >= TC_WALK_MAX) return false;
            seen[n_seen++]   = s;
            stack[n_stack++] = s;
        }
    }
    return false;
}

/* class-superclasses: a declared constraint also declares its superclasses.
 *
 * Entailment used to be decided only at the dispatch site (typeclass_entails),
 * which is enough when the call resolves statically.  It is not enough for a
 * generic that is compiled by DICTIONARY PASSING or interpreted with frame
 * dictionaries: a `[^Alternative F]` body calling `pure` needs an Applicative
 * dictionary, and the generic received only the Alternative one -- the
 * compiled body called `pure` through the Alternative dict's slot (a C type
 * error at best, a wrong method at worst), and turi, which binds dictionaries
 * from this list, had none to find.  Found retrofitting the stdlib's
 * Alternative and Monad (typeclass-superclasses-plan SC8b).
 *
 * So the constraint list itself carries the closure: for each single-parameter
 * constraint `(C W)`, every superclass reachable through single-argument
 * elements over that parameter is appended as `(S W)`, unless the list already
 * has it.  Implied entries go AFTER the declared ones, so code that reads "the
 * first constraint" sees what the author wrote.  The instance obligation
 * (Half B) guarantees every appended dictionary exists at a concrete call
 * site.  Multi-parameter shapes are left to the dispatch-site walk. */
static bool tc_constraint_present(const TypeConstraint *list, uint8_t n,
                                  const TypeConstraint *extra, uint32_t n_extra,
                                  const TypeClass *tc, const Symbol *tyvar) {
    for (uint8_t i = 0; i < n; i++)
        if (tc_same_name(list[i].typeclass, tc) && list[i].tyvar == tyvar)
            return true;
    for (uint32_t i = 0; i < n_extra; i++)
        if (tc_same_name(extra[i].typeclass, tc) && extra[i].tyvar == tyvar)
            return true;
    return false;
}

TypeConstraint *typeclass_constraints_with_supers(const TypeClassEnv *env,
                                                  TypeConstraint *list,
                                                  uint8_t n, uint8_t *out_n) {
    *out_n = n;
    if (!env || !list || n == 0) return list;
    TypeConstraint extra[TC_WALK_MAX];
    uint32_t n_extra = 0;
    for (uint8_t i = 0; i < n; i++) {
        const TypeConstraint *c = &list[i];
        if (!c->typeclass || c->typeclass->n_type_params != 1 || !c->tyvar)
            continue;
        TypeClass *stack[TC_WALK_MAX];
        uint32_t n_stack = 0;
        stack[n_stack++] = c->typeclass;
        while (n_stack > 0) {
            TypeClass *cur = stack[--n_stack];
            for (uint8_t si = 0; si < cur->n_supers; si++) {
                if (cur->super_n_args && cur->super_n_args[si] != 1) continue;
                TypeClass *s = tc_super_at(env, cur, si);
                if (!s || s->n_type_params != 1) continue;
                if (tc_constraint_present(list, n, extra, n_extra, s, c->tyvar))
                    continue;
                if (n + n_extra >= 255 || n_extra >= TC_WALK_MAX ||
                    n_stack >= TC_WALK_MAX)
                    break;
                extra[n_extra] = *c;          /* same tyvar, param, reach */
                extra[n_extra].typeclass = s;
                n_extra++;
                stack[n_stack++] = s;
            }
        }
    }
    if (n_extra == 0) return list;
    TypeConstraint *out = (TypeConstraint *)arena_alloc(
        env->arena, ((size_t)n + n_extra) * sizeof(TypeConstraint));
    if (!out) return list;
    memcpy(out, list, n * sizeof(TypeConstraint));
    memcpy(out + n, extra, n_extra * sizeof(TypeConstraint));
    *out_n = (uint8_t)(n + n_extra);
    return out;
}

/* Recursive colouring: 0 = unvisited, 1 = on the current path, 2 = done. */
static uint8_t tc_cycle_walk(TypeClass *tc, TypeClass **path, uint8_t depth,
                             uint8_t cap, TypeClass **out, uint8_t *out_n) {
    if (depth >= cap) return 0;
    for (uint8_t d = 0; d < depth; d++) {
        if (path[d] == tc) {
            /* Cycle: path[d..depth) then tc again. */
            uint8_t n = 0;
            for (uint8_t k = d; k < depth && n < cap; k++) out[n++] = path[k];
            if (n < cap) out[n++] = tc;
            *out_n = n;
            return 1;
        }
    }
    path[depth] = tc;
    for (uint8_t i = 0; i < tc->n_supers; i++) {
        TypeClass *s = tc->supers ? tc->supers[i] : NULL;
        if (!s) continue;
        if (tc_cycle_walk(s, path, (uint8_t)(depth + 1), cap, out, out_n)) return 1;
    }
    return 0;
}

uint8_t typeclass_find_super_cycle(TypeClass *tc, TypeClass **out, uint8_t cap) {
    if (!tc || tc->n_supers == 0 || cap == 0) return 0;
    TypeClass *path[64];
    uint8_t n = 0;
    if (cap > 64) cap = 64;
    if (tc_cycle_walk(tc, path, 0, cap, out, &n)) return n;
    return 0;
}

/* assoc-types-plan: find the class declaring an associated type `assoc_name`. */
TypeClass *typeclass_env_find_assoc_type(const TypeClassEnv *env,
                                         const Symbol *assoc_name,
                                         uint8_t *out_index) {
    for (TypeClass *tc = env->typeclasses; tc != NULL; tc = tc->next) {
        for (uint8_t k = 0; k < tc->n_assoc_types; k++) {
            if (tc->assoc_type_names && tc->assoc_type_names[k] == assoc_name) {
                if (out_index) *out_index = k;
                return tc;
            }
        }
    }
    return NULL;
}

/* assoc-types-plan: resolve `(assoc_name type_arg)` to its bound concrete Type.
 * Uses precise structural type equality (type_eq) on the single type argument
 * so that distinct instances of the same constructor (Vec int vs Vec cstr) are
 * disambiguated -- the kind-erased dispatch lookup cannot do this. */
const Type *typeclass_env_resolve_assoc_type(const TypeClassEnv *env,
                                             const Symbol *assoc_name,
                                             const Type *type_arg) {
    return typeclass_env_resolve_assoc_type_n(env, assoc_name, type_arg, 1);
}

/* assoc-types-2 (MP4): N-ary projection resolver.  Matches the instance whose
 * arity is `n_type_args` and whose every type arg is structurally equal to the
 * corresponding projection-site type.  The single-arg path (n == 1) is the
 * default and stays the fast common case. */
const Type *typeclass_env_resolve_assoc_type_n(const TypeClassEnv *env,
                                               const Symbol *assoc_name,
                                               const Type *type_args,
                                               uint8_t n_type_args) {
    uint8_t assoc_idx = 0;
    TypeClass *tc = typeclass_env_find_assoc_type(env, assoc_name, &assoc_idx);
    if (!tc || !type_args || n_type_args == 0) return NULL;
    TypeClassInstance *inst =
        typeclass_env_lookup_instance_exact(env, tc, type_args, n_type_args);
    if (!inst) return NULL;
    if (assoc_idx >= inst->n_assoc_types || !inst->assoc_types) return NULL;
    return &inst->assoc_types[assoc_idx];
}

/* assoc-types-2 (MP1): N-ary exact structural instance lookup. */
TypeClassInstance *typeclass_env_lookup_instance_exact(const TypeClassEnv *env,
                                                       const TypeClass *typeclass,
                                                       const Type *type_args,
                                                       uint8_t n_type_args) {
    if (!typeclass || !type_args || n_type_args == 0) return NULL;
    for (TypeClassInstance *inst = env->instances; inst != NULL; inst = inst->next) {
        if (inst->typeclass != typeclass) continue;
        if (inst->n_type_args != n_type_args || !inst->type_args) continue;
        bool all_eq = true;
        for (uint8_t i = 0; i < n_type_args; i++) {
            if (!type_eq(inst->type_args[i], type_args[i])) { all_eq = false; break; }
        }
        if (all_eq) return inst;
    }
    return NULL;
}

/* Look up instances for a typeclass and type arguments.
 *
 * Phase HKT H2: Properly matches type_args by TypeKind (was "first match wins").
 * For KIND_STAR parameters, compares type_args[i].kind element-wise.
 * Falls back to the first matching typeclass if all type_args are unknown (TY_UNKNOWN).
 * This ensures no performance regression for existing KIND_STAR code paths.
 * 
 * Phase PTC3: Check type parameter constraints on matching instances.
 * When an instance has constraints (e.g., Clone[Pair a b] requires [Clone a, Clone b]),
 * verify that the required instances exist for the lookup type arguments.
 */
static TypeClassInstance *typeclass_env_lookup_instance_impl(const TypeClassEnv *env,
                                                              TypeClass *typeclass,
                                                              Type *type_args,
                                                              uint8_t n_type_args);

/* --timings: instance resolution is "typeclass" wherever it is asked for --
 * a constrained call during elaboration, or the CPS pass. */
TypeClassInstance *typeclass_env_lookup_instance(const TypeClassEnv *env,
                                                  TypeClass *typeclass,
                                                  Type *type_args, uint8_t n_type_args) {
    prof_enter(PROF_TYPECLASS);
    TypeClassInstance *r = typeclass_env_lookup_instance_impl(env, typeclass,
                                                              type_args, n_type_args);
    prof_exit(PROF_TYPECLASS);
    return r;
}

static TypeClassInstance *typeclass_env_lookup_instance_impl(const TypeClassEnv *env,
                                                              TypeClass *typeclass,
                                                              Type *type_args,
                                                              uint8_t n_type_args) {
    TypeClassInstance *fallback = NULL;

    for (TypeClassInstance *inst = env->instances; inst != NULL; inst = inst->next) {
        if (inst->typeclass != typeclass) continue;

        /* Arity must match. */
        if (inst->n_type_args != n_type_args) continue;

        /* Match element-wise by TypeKind (kind erasure: only the top-level kind
         * is compared; inner types are not inspected in H2).
         *
         * F3-7 (cross-plan-followups): TY_APP receivers (e.g. Vec[int],
         * Vec[Vec[int]]) need to match TY_STRUCT instances (e.g. Eq[Vec])
         * because the type constructor head IS a struct.  Treat
         * TY_APP receiver as if it had the head struct's kind for the
         * primary match; the subsequent constraint-satisfaction check
         * (below) walks the TY_APP's args to validate inner constraints
         * recursively. */
        bool match = true;
        bool all_unknown = (n_type_args == 0);
        for (uint8_t i = 0; i < n_type_args; i++) {
            if (type_args[i].kind == TY_UNKNOWN) {
                /* Caller does not know the type yet; defer to fallback. */
                all_unknown = true;
                break;
            }
            TypeKind eff_kind = type_args[i].kind;
            const AdtDef *eff_adt_def = NULL;
            if (eff_kind == TY_APP) {
                /* Walk to the head of the TY_APP chain (a type constructor --
                 * a record defadt once a parametric struct lowers). */
                const Type *head = &type_args[i];
                while (head && head->kind == TY_APP) head = head->as.app.fn;
                if (head && head->kind == TY_ADT) {
                    /* CONV-S1: an ADT-app receiver (e.g. `(Option int)` after
                     * Option lowers) normalises to its TY_ADT head so it matches
                     * a TY_ADT-headed instance and -- via eff_adt_def below --
                     * discriminates between distinct ADT constructors instead of
                     * collapsing every ADT-app to a bare kind-TY_APP match (which
                     * mis-selected the first TY_APP instance, e.g. MutableMap). */
                    eff_kind = TY_ADT;
                    eff_adt_def = head->as.adt_.def;
                }
            } else if (eff_kind == TY_ADT) {
                eff_adt_def = type_args[i].as.adt_.def;
            }
            /* Normalize the INSTANCE head the same way as the query head above.
             * An instance declared with a (partially/fully) applied head -- e.g.
             * `[(Option A)]` -- stores its type_arg as TY_APP(Option, ...),
             * whereas a bare head -- `[Vec]` -- stores it as TY_STRUCT.  Walk a
             * struct-headed TY_APP instance down to its TY_STRUCT constructor so
             * a concrete query like (Option int) (normalized to TY_STRUCT Option
             * above) matches it.  Without this symmetry a NESTED-container
             * constraint discharge -- the `(C (Option int))` required by a
             * `(Vec (Option int))` receiver against a `C [Vec] [(C A)]` instance
             * -- fails: the query side is TY_STRUCT while the instance side is
             * the raw TY_APP, so the kinds never line up and dispatch reports a
             * spurious TUR_E0020 ambiguity. */
            TypeKind inst_eff_kind = inst->type_args[i].kind;
            const AdtDef *inst_eff_adt_def = NULL;
            if (inst_eff_kind == TY_APP) {
                const Type *ihead = &inst->type_args[i];
                while (ihead && ihead->kind == TY_APP) ihead = ihead->as.app.fn;
                if (ihead && ihead->kind == TY_ADT) {
                    inst_eff_kind = TY_ADT;
                    inst_eff_adt_def = ihead->as.adt_.def;
                }
            } else if (inst_eff_kind == TY_ADT) {
                inst_eff_adt_def = inst->type_args[i].as.adt_.def;
            }
            if (inst_eff_kind != eff_kind) {
                match = false;
                break;
            }
            /* CONV-S1: the same identity check for ADT constructors, so a lowered
             * `(Option int)` matches `Eq [Option]` and not, say, `Eq [Result]` or
             * `Eq [MutableMap]` (both ADT/TY_APP-kinded after lowering). */
            if (eff_adt_def &&
                inst_eff_kind == TY_ADT &&
                inst_eff_adt_def != NULL &&
                inst_eff_adt_def != eff_adt_def) {
                match = false;
                break;
            }
        }
        if (all_unknown) {
            /* Record as fallback — used only when no typed match is found. */
            if (!fallback) fallback = inst;
            continue;
        }
        if (!match) continue;
        
        /* Phase PTC3/PTC4: Check type parameter constraints on this instance.
         * Extract concrete element types from TY_APP for param_idx substitution.
         * For multi-param types (Map[K V] → TY_APP(TY_APP(Map,K),V)), collect
         * args in type_params order (innermost first) by reversing the app chain. */
        {
            const Type *elem_types = NULL;
            uint8_t n_elem = 0;
            Type elem_buf[8];
            if (n_type_args > 0 && type_args[0].kind == TY_APP) {
                Type raw[8];
                uint8_t n_raw = 0;
                for (const Type *tx = &type_args[0];
                     tx && tx->kind == TY_APP && n_raw < 8;
                     tx = tx->as.app.fn) {
                    if (tx->as.app.arg) raw[n_raw++] = *tx->as.app.arg;
                }
                for (uint8_t ri = 0; ri < n_raw; ri++)
                    elem_buf[ri] = raw[n_raw - 1 - ri];
                n_elem = n_raw;
                if (n_elem > 0) elem_types = elem_buf;
            }
            if (!typeclass_instance_constraints_satisfied(inst, type_args, n_type_args,
                                                          elem_types, n_elem, env)) {
                continue;
            }
        }
        
        return inst;
    }

    return fallback;
}

/* Initialize a constraint set */
void constraint_set_init(ConstraintSet *cs) {
    cs->constraints = NULL;
    cs->n_constraints = 0;
    cs->cap_constraints = 0;
}

/* Add a constraint to a set */
void constraint_set_add(ConstraintSet *cs, TypeClass *typeclass, Type type_arg) {
    if (cs->n_constraints >= cs->cap_constraints) {
        cs->cap_constraints = cs->cap_constraints ? cs->cap_constraints * 2 : 8;
        cs->constraints = (TypeConstraint *)realloc(cs->constraints,
                                                   cs->cap_constraints * sizeof(TypeConstraint));
        if (!cs->constraints) return;
    }
    cs->constraints[cs->n_constraints].typeclass = typeclass;
    cs->constraints[cs->n_constraints].type_arg = type_arg;
    cs->constraints[cs->n_constraints].param_idx = -1;
    cs->constraints[cs->n_constraints].tyvar = NULL;
    cs->constraints[cs->n_constraints].return_resolved = false;
    cs->constraints[cs->n_constraints].n_arg_tyvars = 0;
    cs->n_constraints++;
}

/* Free a constraint set */
void constraint_set_free(ConstraintSet *cs) {
    free(cs->constraints);
    cs->constraints = NULL;
    cs->n_constraints = 0;
    cs->cap_constraints = 0;
}

/* Check if a type satisfies all constraints in a set */
bool constraint_set_satisfied(const ConstraintSet *cs, Type type, const TypeClassEnv *env) {
    for (uint8_t i = 0; i < cs->n_constraints; i++) {
        TypeClassInstance *inst = typeclass_env_lookup_instance(
            env, cs->constraints[i].typeclass, &type, 1);
        if (!inst) {
            return false;
        }
    }
    return true;
}

/* Phase PTC3: Check if an instance's type parameter constraints are satisfied
 * for the given type arguments.
 * 
 * For example, if instance is Clone[Pair a b] with constraints [Clone a, Clone b],
 * and we're looking up Clone[Pair int bool], we need to check that
 * Clone[int] and Clone[bool] instances exist.
 * 
 * The instance's type_param_constraints are stored as TypeConstraint array.
 * Each TypeConstraint has a typeclass and a type_arg. The type_arg may be:
 *   - A primitive type (int, bool, etc.) - check directly
 *   - A type parameter of the instance - substitute with the corresponding type_arg
 *     from the lookup (Phase PTC4).
 * 
 * In PTC3 v1: we only handle concrete type constraints (not type parameter
 * substitution), which is sufficient for instances like Clone[MyPair] [Clone int].
 */
bool typeclass_instance_constraints_satisfied(const TypeClassInstance *inst,
                                              Type *lookup_type_args, uint8_t n_lookup_args,
                                              const Type *concrete_elem_types, uint8_t n_concrete,
                                              const TypeClassEnv *env) {
    if (!inst->type_param_constraints || inst->n_type_param_constraints == 0) {
        return true;
    }

    for (uint8_t i = 0; i < inst->n_type_param_constraints; i++) {
        TypeClass *required_tc = inst->type_param_constraints[i].typeclass;
        int8_t pidx = inst->type_param_constraints[i].param_idx;
        Type required_type;
        /* PTC4: if param_idx >= 0, substitute with concrete element type from TY_APP */
        if (pidx >= 0 && concrete_elem_types && (uint8_t)pidx < n_concrete) {
            required_type = concrete_elem_types[(uint8_t)pidx];
        } else {
            required_type = inst->type_param_constraints[i].type_arg;
        }
        /* M5 (docs/archive/history/m5-constrained-poly-spec-wrong-dispatch-for-
         * parametric-receiver.md): when the substituted required_type is a
         * type variable, we're at elab time inside a constrained-polymorphic
         * defn whose own constraints (e.g. `(Eq A)` on `defn f [A] [(Eq A)]
         * [x : (Vec A) ...]`) guarantee the instance will exist at every
         * monomorphization site.  Tentatively accept; the outer call-site
         * elab resolves A to a concrete type and the spec emit re-resolves
         * the dispatch through that concrete A.  Without this, `Eq Vec` was
         * silently dropped for a `(Vec A)` receiver and the dispatch fell
         * through to a primitive-instance fallback (TUR-E0020 ambiguity
         * post-fix; wrong-instance silent miscompile pre-fix). */
        if (required_type.kind == TY_TYVAR) continue;
        TypeClassInstance *constraint_inst = typeclass_env_lookup_instance(
            env, required_tc, &required_type, 1);
        if (!constraint_inst) {
            return false;
        }
    }

    return true;
}

/* Solve constraints for a function call */
bool typeclass_solve_constraints(ConstraintSet *constraints, Type *arg_types, uint8_t n_args,
                                  const TypeClassEnv *env) {
    /* For each constraint, check if there's a matching instance */
    for (uint8_t i = 0; i < constraints->n_constraints; i++) {
        TypeClass *tc = constraints->constraints[i].typeclass;
        Type type_arg = constraints->constraints[i].type_arg;
        
        /* Try to find an instance for this type argument */
        TypeClassInstance *inst = typeclass_env_lookup_instance(env, tc, &type_arg, 1);
        if (!inst) {
            return false; /* No instance found */
        }
    }
    return true;
}

/* Look up an instance using a structured TypeClassDispatchKey (Phase HKT H2).
 *
 * Two-level lookup:
 *   Level 1 — constructor kind (KIND_STAR vs KIND_ARROW).
 *   Level 2 — for KIND_STAR: exact TypeKind match on type_args.
 *             for KIND_ARROW: match by typeclass only (constructor-name keying
 *             is reserved for Phase H3+).
 *
 * No performance regression for the all-KIND_STAR fast path: KIND_STAR simply
 * delegates to typeclass_env_lookup_instance().
 */
TypeClassInstance *typeclass_env_lookup_instance_by_key(const TypeClassEnv *env,
                                                         const TypeClassDispatchKey *key) {
    if (!key || !key->typeclass) return NULL;

    /* Fast path: KIND_STAR — delegate to the typed lookup. */
    if (key->constructor_kind == KIND_STAR) {
        return typeclass_env_lookup_instance(env, key->typeclass,
                                             key->type_args, key->n_type_args);
    }

    /* KIND_ARROW path: find the first instance whose first type_arg is a
     * non-primitive (struct or user-defined type, effective kind * -> *).
     * Phase H3 will add constructor-name keying for Functor, Monad, etc. */
    for (TypeClassInstance *inst = env->instances; inst != NULL; inst = inst->next) {
        if (inst->typeclass != key->typeclass) continue;
        if (inst->n_type_args != key->n_type_args) continue;
        if (inst->n_type_args > 0) {
            TypeKind tk = inst->type_args[0].kind;
            /* M5 fix: include sized numeric variants (mirrors elab_typeclasses.c). */
            bool is_primitive = (tk == TY_INT  || tk == TY_BOOL  || tk == TY_CSTR ||
                                 tk == TY_NIL  || tk == TY_FLOAT || tk == TY_PTR_VOID ||
                                 tk == TY_SYM  ||
                                 tk == TY_INT8 || tk == TY_INT16 || tk == TY_INT32 ||
                                 tk == TY_INT64 ||
                                 tk == TY_UINT8 || tk == TY_UINT16 || tk == TY_UINT32 ||
                                 tk == TY_UINT64 ||
                                 tk == TY_FLOAT32 || tk == TY_FLOAT64);
            if (!is_primitive) return inst;
        } else {
            return inst;
        }
    }
    return NULL;
}
