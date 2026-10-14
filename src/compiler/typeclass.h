#ifndef TUR_TYPECLASS_H
#define TUR_TYPECLASS_H

#include <stdint.h>
#include <stdbool.h>
#include "arena.h"
#include "symbols.h"
#include "types.h"

/* Forward declaration for EffectRow (defined in effect.h) */
struct EffectRow;
/* Forward declaration for Binding (defined in expr.h) */
struct Binding;

/* Forward declarations */
typedef struct TypeClassMethod TypeClassMethod;
typedef struct TypeConstraint TypeConstraint;

/* A method in a typeclass */
struct TypeClassMethod {
    const Symbol *name;           /* Method name */
    const Symbol **param_names;   /* Parameter names */
    Type *param_types;           /* Parameter types */
    /* Phase CCL: per-parameter callable flag.  param_is_fn[j] = true means the
     * j-th parameter is a single-argument callable and should be represented as
     * tur_poly_fn_t at call sites so that capturing closures can be passed.
     * Set by parse_typeclass_method when the defclass uses [param :fn] syntax.
     * NULL means no parameters are callable (backwards-compatible default). */
    bool *param_is_fn;
    /* Prereq 4: per-parameter flag tracking whether the user wrote an explicit
     * type annotation on the j-th parameter (e.g. `[v : int]`). When the user
     * did NOT annotate, `param_types[j]` defaults to TYPE_INT, which is
     * indistinguishable from a literal `:int` annotation. Without this flag,
     * the elaborator's instance-side rewrite at elab_definstance (line ~2377)
     * blindly substitutes the class tyvar into both shapes, silently
     * mistyping explicit-`:int` params (the symptom that blocked the typed
     * Decode surface for the json spice). Set by parse_typeclass_method when
     * an explicit type annotation is present; NULL means no params had
     * explicit annotations (backwards-compatible default). */
    bool *param_explicit_type;
    /* RT1: the refinement each parameter declares in the CLASS signature, or
     * NULL.  An instance's own refinement is checked against this: the class
     * signature is the promise callers program against, so an instance that
     * demands MORE than the class does would reject an argument a generic
     * caller was entitled to pass.  Arrays have n_params entries. */
    const struct Form **param_refine_preds;
    const char        **param_refine_vars;
    /* RT1: lazily-built Binding carrying the CLASS's parameter refinements, so
     * a DYNAMIC dispatch (no statically-resolved instance) can record an
     * ordinary crossing against the class signature -- the strongest demand
     * true of every instance, since `TUR-E0374` forbids an instance demanding
     * more.  Cached here rather than rebuilt per call site because crossings
     * deduplicate on (callee, call_form): a fresh Binding each time would
     * defeat that and double-report a re-elaborated site.  NULL until the
     * first dynamic dispatch that needs it, and never built when no parameter
     * carries a refinement. */
    struct Binding *refine_class_binding;
    uint8_t n_params;
    Type return_type;             /* Return type (contract PEELED to its base) */
    /* RT1: the refinement the CLASS signature declares on the method's RESULT,
     * or NULL.  Parameters and results vary in OPPOSITE directions: an instance
     * may accept more than the class promises, but it must deliver at least as
     * much.  So a class result refinement is inherited by an instance that does
     * not restate one, and an instance that does restate one owes
     * `instance_pred(r) |- class_pred(r)`. */
    const struct Form  *return_refine_pred;
    const char         *return_refine_var;
    /* ER3: Effect-row annotation from the defclass method signature.
     * NULL if not annotated; ERK_UNRESOLVED until PASS_EFFECT_ROW_INFER resolves it. */
    struct EffectRow *effect_row;
    /* ER3: Elaborated default method body as a file-level EX_FN_DEF expression.
     * NULL if the defclass method has no default body.  Registered in the program
     * by elab_defclass so effect_check_pass validates it automatically. */
    struct Expr *default_fn_expr;
    /* typeclass-default-methods-do-not-work: the class's method FORM when it
     * carries a default body -- `(lte? [x y] : bool (or (.lt? x y) (= x y)))`
     * -- kept UNELABORATED.  A default cannot be elaborated at the class: no
     * instance exists yet, so a sibling call has nothing to resolve against,
     * and the eager attempt broke every class that merely declared one.  It is
     * elaborated per instance instead, by elab_definstance splicing this form
     * in for a method the instance omits, so it goes through the ordinary
     * instance-method path with the receiver at the instance's own type. */
    const struct Form *default_method_form;
};

/* A typeclass definition (e.g., Eq, Ord, Show) */
struct TypeClass {
    const Symbol *name;           /* Typeclass name */
    const Symbol **type_params;   /* Type parameters (e.g., [a] for Eq[a]) */
    /* Phase HKT (v2, stub): kind annotation per type parameter.
     * Always KIND_STAR in v1; NULL treated as all-KIND_STAR. */
    Kind         *type_param_kinds; /* Parallel to type_params; may be NULL in v1 */
    uint8_t n_type_params;
    TypeClassMethod *methods;    /* Methods in this typeclass */
    uint8_t n_methods;
    /* Associated type members (assoc-types-plan): zero or more associated type
     * projections declared with `(type Name : Type)` in the defclass body.
     * Each instance binds them to a concrete Type (see assoc_types below).
     * Parallel to length n_assoc_types; NULL when the class declares none. */
    const Symbol **assoc_type_names;
    uint8_t        n_assoc_types;
    /* assoc-types-2 (Part A / MP2): a single functional dependency declared with
     * the `| (from... -> to...)` clause after the type-param vector.  The masks
     * are bitsets over type-parameter indices (bit i set => param i participates).
     * `has_fundep` is false when no `|` clause is present (every parameter must
     * then be fixed at the dispatch site).  v1 stores at most one fundep; widen
     * to an array only on demand (see plan Open Question 2). */
    bool     has_fundep;
    uint16_t fundep_from_mask;
    uint16_t fundep_to_mask;
    /* class-superclasses (docs/archive/typeclass-superclasses-plan.md, SC1;
     * gated behind --enable=class-superclasses): the constraint preamble
     * written between the type-param vector and the fundep clause --
     * `(defclass Monoid [a] [(Semigroup a)] ...)`.  Each element is kept as
     * its UNRESOLVED `(Super var...)` form, because a superclass may be
     * declared below its subclass (the same reason default_method_form stays
     * unelaborated); `super_arg_idx` records, per element, which of THIS
     * class's type params each argument names (stride TUR_SUPER_MAX_ARGS).
     * `supers` is filled by typeclass_resolve_superclasses in the post-unit
     * pass; before that, typeclass_entails resolves by name on demand. */
#define TUR_SUPER_MAX_ARGS 4
    const struct Form **super_forms;
    uint8_t            *super_n_args;
    uint8_t            *super_arg_idx;
    uint8_t             n_supers;
    TypeClass         **supers;
    /* The defclass form, for diagnostics raised in the post-unit pass. */
    const struct Form  *decl_form;
    /* Phase HKT-P4: file that defined this typeclass (for orphan instance check).
     * file_id mirrors Span.file_id; 0 means unknown/builtin. */
    uint16_t origin_file_id;
    /* method-vs-defn clash check: true if this typeclass was registered while
     * an auto-loaded stdlib module was being elaborated.  Overriding a stdlib
     * class method with a same-named free defn is a documented, intentional
     * pattern, so the TUR-W0039 clash warning fires only for user-defined
     * classes (from_stdlib == false).  See
     * docs/archive/history/typeclass-methods-share-value-namespace-with-defns.md. */
    bool from_stdlib;
    /* For linking */
    TypeClass *next;             /* Next typeclass in global registry */
};

/* A typeclass instance (e.g., Eq for int) */
struct TypeClassInstance {
    TypeClass *typeclass;         /* The typeclass this is an instance of */
    Type *type_args;              /* Concrete types for type parameters */
    /* Phase HKT §1: original symbol name for each type arg (e.g. "option",
     * "vec").  Parallel to type_args; entries may be NULL for primitive types
     * that have an unambiguous C name from their TypeKind.  Used by emit.c to
     * produce distinct dictionary struct names (dict_Functor_option vs
     * dict_Functor_vec) when multiple HKT instances exist. */
    const struct Symbol **type_arg_syms;
    uint8_t n_type_args;
    /* Method implementations - these are FnDef pointers */
    struct FnDef **method_impls;   /* Function definitions for each method */
    uint8_t n_method_impls;
    /* saffron-lang-plan S9 / D8 Q3: per-method-slot WITNESS defns for dynamic
     * dispatch on a parametric (HKT) receiver, or NULL.  A witness is an
     * ordinary Saffron-span defn the elaborator synthesises on demand --
     * `(defn __dynwit_Functor_fmap_Option [__r : (Option any) __a1] : any
     * (.fmap __r __a1))` -- so the STATIC path mints the by-value spec for the
     * all-`any` instantiation and the D5 seams do the argument typing; the
     * registry row's shim then calls it.  Never the carrier base, which reads
     * an `(Option any)`'s 16-byte element as an int64. */
    struct FnDef **dyn_witness;
    /* Constraints on type parameters (e.g., Eq a => Eq (List a)) */
    TypeClassInstance **constraints;
    uint8_t n_constraints;
    /* Phase PTC1: Type parameter constraints (e.g., [Clone a, Clone b] in Clone [Pair a b])
     * These are constraints that the type parameters must satisfy.
     * Stored as TypeConstraint array for better type info tracking. */
    TypeConstraint *type_param_constraints;
    uint8_t n_type_param_constraints;
    /* Associated type bindings (assoc-types-plan): parallel to the class's
     * assoc_type_names.  assoc_types[k] is the concrete Type this instance
     * binds for the class's k-th associated type member, so a type-level
     * projection `(Name <inst-type-arg>)` resolves to assoc_types[k].
     * NULL/0 when the class declares no associated types. */
    Type   *assoc_types;
    uint8_t n_assoc_types;
    /* Phase HKT-P4: file that defined this instance (for orphan instance check).
     * file_id mirrors Span.file_id; 0 means unknown. */
    uint16_t origin_file_id;
    /* class-superclasses SC4: the definstance form (for the obligation
     * diagnostic), and whether the obligation has already been discharged --
     * a REPL session re-runs the post-unit pass every turn. */
    const struct Form *decl_form;
    bool super_obligations_ok;
    /* M7 partial-application wildcard head (e.g. `(Result _ B)`): the index of
     * the `_` hole slot within the constructor's type params (0-based), so the
     * by-value HKT grounding can fix the OTHER (non-hole) slots from the
     * concrete receiver while the hole slot is grounded by the mapped element.
     * 0xFF means "no wildcard head" (a bare ctor or a fully-fixed app). */
    uint8_t partial_hole_pos;
    /* saffron-lang-plan S9 (D8 Q1): true for a `C [any]` instance the
     * elaborator MINTED for a dynamic program, whose every method body
     * dispatches on the receiver box's tag.  It exists so a constrained
     * instance -- `(definstance Show [Vec] [(Show A)] ...)` -- has a
     * dictionary to discharge `(Show A)` with when `A = any`, which is the
     * only instantiation a dynamic dialect builds.  An `any` receiver never
     * resolves to it STATICALLY (that would be its own body calling itself);
     * the any-receiver arm of elab_method_call treats it as absent and
     * dispatches on the tag instead.  A hand-written `C [any]` (Hash, MapKey)
     * is a real instance and leaves this false. */
    bool dyn_any_minted;
    /* For linking */
    TypeClassInstance *next;      /* Next instance in global registry */
};

/* Typeclass environment (global registry) */
typedef struct TypeClassEnv {
    Arena *arena;
    TypeClass *typeclasses;
    TypeClassInstance *instances;
} TypeClassEnv;

/* Initialize typeclass environment */
void typeclass_env_init(TypeClassEnv *env, Arena *arena);

/* Register a typeclass */
TypeClass *typeclass_env_register_typeclass(TypeClassEnv *env, const Symbol *name);

/* Register a typeclass instance */
TypeClassInstance *typeclass_env_register_instance(TypeClassEnv *env, TypeClass *typeclass);

/* Look up a typeclass by name */
TypeClass *typeclass_env_lookup_typeclass(const TypeClassEnv *env, const Symbol *name);

/* class-superclasses SC2/SC3: does a constraint on `sub` entail `sup`?  True
 * when the two are the same class (by identity, or by NAME so a class
 * re-registered through two import paths still counts) or when `sup` is in
 * `sub`'s transitive superclass closure.  Superclasses not yet resolved by the
 * post-unit pass are looked up by name on demand, so this works mid-unit
 * (a superclass declared BELOW its subclass resolves once it is registered);
 * a cyclic graph is walked with a visited set and reported elsewhere. */
bool typeclass_entails(const TypeClassEnv *env, TypeClass *sub, const TypeClass *sup);
/* class-superclasses: `list` with each single-parameter constraint's
 * superclass closure appended (same tyvar), skipping any already present.
 * Returns `list` itself when nothing is added. */
TypeConstraint *typeclass_constraints_with_supers(const TypeClassEnv *env,
                                                  TypeConstraint *list,
                                                  uint8_t n, uint8_t *out_n);

/* class-superclasses SC2: resolve `tc->super_forms` to `tc->supers` by name.
 * Returns false (leaving the unresolved slot NULL) when a name is unknown;
 * the caller reports it.  Idempotent. */
bool typeclass_resolve_superclasses(const TypeClassEnv *env, TypeClass *tc);

/* class-superclasses SC2: find a superclass cycle reachable from `tc`.
 * Returns the number of classes on the cycle path written to `path` (at most
 * `cap`, the first and last entries being the same class), or 0 when the
 * graph under `tc` is acyclic.  Only follows RESOLVED supers. */
uint8_t typeclass_find_super_cycle(TypeClass *tc, TypeClass **path, uint8_t cap);

/* RT4: find the class METHOD a bare name denotes, if any.  The refinement
 * encoder needs this to tell a typeclass method apart from a name that
 * resolves to nothing at all -- the latter is an abstract measure, which the
 * language defines as congruent, and a method is emphatically not one.
 * Writes the declaring class to *out_class when non-NULL. */
const TypeClassMethod *typeclass_env_find_method(const TypeClassEnv *env,
                                                 const Symbol *name,
                                                 const TypeClass **out_class);

/* assoc-types-plan: find the typeclass that declares an associated type member
 * named `assoc_name`, writing the member's index within that class to
 * *out_index.  Returns NULL (leaving *out_index untouched) when no registered
 * class declares an associated type with that name. */
TypeClass *typeclass_env_find_assoc_type(const TypeClassEnv *env,
                                         const Symbol *assoc_name,
                                         uint8_t *out_index);

/* assoc-types-plan: resolve the type-level projection `(assoc_name type_arg)`
 * to the concrete Type the matching instance binds for that associated type.
 * `type_arg` is the class's single type argument as written at the projection
 * site (e.g. `(Vec int)` in `(Elem (Vec int))`).  Instances are matched by
 * precise structural type equality, so `(Elem (Vec int))` and
 * `(Elem (Vec cstr))` resolve to distinct bindings.  Returns NULL when no
 * class declares `assoc_name`, no instance precisely matches `type_arg`, or
 * the instance left the member unbound. */
const Type *typeclass_env_resolve_assoc_type(const TypeClassEnv *env,
                                             const Symbol *assoc_name,
                                             const Type *type_arg);

/* assoc-types-2 (Part A / MP4): N-ary form of the projection resolver.  For an
 * N-parameter class, the projection `(AssocName T1 T2 ... Tn)` matches the
 * instance whose `n_type_args == n` and whose every type arg is `type_eq` to
 * the corresponding `type_args[i]`.  The single-arg `typeclass_env_resolve_
 * assoc_type` is the n == 1 special case and delegates here.  Returns NULL when
 * no class declares `assoc_name`, no instance matches all n args, or the
 * matched instance left the member unbound. */
const Type *typeclass_env_resolve_assoc_type_n(const TypeClassEnv *env,
                                               const Symbol *assoc_name,
                                               const Type *type_args,
                                               uint8_t n_type_args);

/* assoc-types-2 (Part A / MP1): N-ary *exact* instance lookup.  Unlike
 * typeclass_env_lookup_instance (kind-erased, first-match), this matches all
 * `n_type_args` arguments by precise structural equality (`type_eq`), so it
 * disambiguates distinct instances of the same constructor (Collect [(Vec int)
 * int] vs Collect [(Vec cstr) cstr]).  Returns the matching instance or NULL.
 * Does not consult type-parameter constraints -- it is a precise structural
 * matcher, the substrate for fundep propagation and multi-param projection. */
TypeClassInstance *typeclass_env_lookup_instance_exact(const TypeClassEnv *env,
                                                       const TypeClass *typeclass,
                                                       const Type *type_args,
                                                       uint8_t n_type_args);

/* Look up instances for a typeclass and type arguments */
TypeClassInstance *typeclass_env_lookup_instance(const TypeClassEnv *env,
                                                  TypeClass *typeclass,
                                                  Type *type_args, uint8_t n_type_args);

/* Phase PTC3/PTC4: Check if an instance's type parameter constraints are satisfied.
 * concrete_elem_types/n_concrete: elem types extracted from TY_APP at dispatch;
 * used to substitute constraints where param_idx >= 0. Pass NULL/0 when unavailable. */
bool typeclass_instance_constraints_satisfied(const TypeClassInstance *inst,
                                              Type *lookup_type_args, uint8_t n_lookup_args,
                                              const Type *concrete_elem_types, uint8_t n_concrete,
                                              const TypeClassEnv *env);

/* Phase HKT H2: Structured dispatch-table key.
 * Generalized lookup key carrying the constructor kind for two-level dispatch.
 * KIND_STAR keys fall through to typeclass_env_lookup_instance (no regression). */
typedef struct TypeClassDispatchKey {
    TypeClass       *typeclass;
    Type            *type_args;
    uint8_t          n_type_args;
    /* HKT extension: KIND_STAR for concrete types, KIND_ARROW for type constructors. */
    Kind             constructor_kind;
} TypeClassDispatchKey;

/* Phase HKT H2: Look up an instance using a structured TypeClassDispatchKey.
 * Two-level dispatch: constructor kind (level 1) then type_args (level 2).
 * For KIND_STAR keys this delegates to typeclass_env_lookup_instance(). */
TypeClassInstance *typeclass_env_lookup_instance_by_key(const TypeClassEnv *env,
                                                         const TypeClassDispatchKey *key);

/* Constraint on a type variable */
typedef struct TypeConstraint {
    TypeClass *typeclass;
    Type       type_arg;   /* concrete type (used when param_idx < 0) */
    int8_t     param_idx;  /* >= 0: index into TY_APP elem types; -1: use type_arg directly */
    /* Phase RT: the type variable this constraint quantifies over (from a
     * `where (Class tyvar)` clause).  NULL for legacy `^Class`-style
     * constraints.  return_resolved is true when `tyvar` appears only in the
     * function's return type (no parameter carries it), so the constraint must
     * be resolved from the expected result type rather than an argument. */
    const struct Symbol *tyvar;
    bool                 return_resolved;
    /* declared-tyvars-are-not-rigid: EVERY variable of a middle-vector
     * constraint `[(StorageOps S E)]`, in class-parameter order (`[0]` is
     * `tyvar`).  A method whose declared result is the class's second
     * parameter is typed from here inside the constrained generic -- the
     * representative instance's concrete type is not the body's.  The caret
     * forms record none. */
    const struct Symbol *arg_tyvars[8];
    uint8_t              n_arg_tyvars;
} TypeConstraint;

/* Constraint set for a function */
typedef struct ConstraintSet {
    TypeConstraint *constraints;
    uint8_t n_constraints;
    uint8_t cap_constraints;
} ConstraintSet;

/* Initialize a constraint set */
void constraint_set_init(ConstraintSet *cs);

/* Add a constraint to a set */
void constraint_set_add(ConstraintSet *cs, TypeClass *typeclass, Type type_arg);

/* Free a constraint set */
void constraint_set_free(ConstraintSet *cs);

/* Check if a type satisfies all constraints in a set */
bool constraint_set_satisfied(const ConstraintSet *cs, Type type, const TypeClassEnv *env);

/* Solve constraints for a function call */
bool typeclass_solve_constraints(ConstraintSet *constraints, Type *arg_types, uint8_t n_args,
                                  const TypeClassEnv *env);

#endif
