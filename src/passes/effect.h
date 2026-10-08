#ifndef TUR_EFFECT_H
#define TUR_EFFECT_H

#include <stdint.h>
#include <stdbool.h>
#include "arena.h"
#include "symbols.h"
#include "types.h"  /* For TypeKind */
#include "buf.h"    /* For Buf */

/* Forward declarations */
typedef struct Effect Effect;
typedef struct EffectConstructor EffectConstructor;
struct EffectRow; /* Defined in types.h as forward declaration */
typedef struct EffectRow EffectRow;

/* Effect constructor represents a single effect operation.
 * e.g., (defeffect Read [prompt : cstr] : str) creates an effect with
 * one constructor that takes a cstr parameter and returns a str. */
struct EffectConstructor {
    const Symbol *name;           /* Name of the constructor (e.g., Read) */
    const Symbol **param_names;  /* Parameter names */
    TypeKind *param_types;       /* Parameter types */
    uint8_t n_params;
    TypeKind result_type;        /* Result type of the effect operation */
    Effect *effect;              /* Parent effect (for now, 1:1 effect:constructor) */
    /* Tier C (cps-backend): the full result / parameter Types, when they are
     * aggregates whose `TypeKind` above loses the def (a by-value struct/ADT).
     * NULL when scalar (the TypeKind is sufficient).  Set post-registration by
     * defeffect elaboration; read by perform (result) and handle (params) so a
     * by-value aggregate effect value carries its real monomorphized def. */
    const struct Type *result_full_type;
    const struct Type **param_full_types;
    /* cps-dk-multishot-user-effects (Phase A): index of a RESUMABLE fn PAYLOAD
     * param -- a `(fn [effect-cont] R)` whose first arg is a continuation, so the
     * effect is resumed THROUGH the payload (`(E [f] k) (f k)`).  Detected at
     * defeffect from the param type FORM (the `effect-cont` cont flavor collapses
     * to its TY_INT carrier in the stored Type, so the form is the reliable
     * signal).  -1 when the effect has no such param.  Read by the perform reflavor
     * and the handler-case CK_MULTISHOT upgrade so both halves share the DK-backed
     * cloneable-cont substrate. */
    int resumable_payload_param;
};

/* Effect represents a user-defined effect type.
 * In v3, effects are algebraic and can have multiple constructors,
 * but for simplicity in Phase 19 v1, we start with single-constructor effects. */
struct Effect {
    const Symbol *name;           /* Effect name */
    EffectConstructor *constructor; /* The effect constructor */
    bool is_polymorphic;          /* Whether this effect is generic over type parameters */
    /* Phase P19-6: Module visibility */
    bool          is_private;           /* declared with ^private — only visible within defining module */
    bool          is_exported;          /* false only for ^private effects; true for all public ones */
    const Symbol *defining_module_name; /* module that declared this effect, or NULL for top-level */
    /* ET4: effect hierarchy -- NULL if no parent */
    struct Effect *parent;
    /* Capability effect (stdlib-effect-rows): a coarse capability tag (e.g.
     * #{FS}, #{Net}) that marks a function's authority rather than an effect it
     * `perform`s.  Capability effects are justified by their declared annotation
     * alone -- they are never inferred from the body, never warned as
     * over-annotated (TUR-W0031), and their *declared* presence on a callee
     * propagates into the caller's inferred row (like the built-in Unsafe). */
    bool is_capability;
};

/* Effect row represents a set of effects that a function may perform.
 * This is the core of the effect system - functions have effect rows in their types.
 * 
 * In v3, effect rows are either:
 * - Empty: {} - pure function
 * - Concrete: {Effect1, Effect2} - specific effects
 * - Polymorphic: {a} - effect variable
 * 
 * For Phase 19 v1, we start with concrete effect sets. */
typedef enum EffectRowKind {
    ERK_EMPTY,       /* {} - no effects */
    ERK_CONCRETE,    /* {E1, E2, ...} - specific effect set */
    ERK_VAR,         /* {a} - effect variable (lowercase name = row variable) */
    ERK_UNION,       /* {E1 | E2} - union (for type inference) */
    ERK_UNRESOLVED,  /* #{Foo Bar e} - symbolic names parsed during elab; resolved
                      * to ERK_CONCRETE/ERK_VAR after PASS_EFFECT_LOWER by
                      * effect_row_resolve(). Uppercase names → concrete effects;
                      * lowercase names → ERK_VAR row variables. */
} EffectRowKind;

struct EffectRow {
    EffectRowKind kind;
    union {
        struct {
            Effect **effects;    /* Array of effects in this row */
            uint8_t n_effects;
        } concrete;
        struct {
            const Symbol *var_name;  /* Effect variable name */
        } var;
        struct {
            EffectRow *left;
            EffectRow *right;
        } union_;
        struct {
            const Symbol **sym_names; /* Unresolved symbolic names */
            uint8_t n_sym_names;
            /* The `#fx{...}` form the names came from, so a name no
             * `defeffect` declares can be reported where it was written
             * (TUR-E0026).  Zero for a row with no source form. */
            Span span;
            /* Set once this row's unknown names have been reported: a row
             * object reached through two holders is resolved twice, and
             * must not be diagnosed twice. */
            bool unknown_reported;
        } unresolved;
    } as;
};

/* Create an empty effect row */
EffectRow *effect_row_empty(Arena *a);

/* Create a concrete effect row with a single effect */
EffectRow *effect_row_single(Arena *a, Effect *effect);

/* Create a concrete effect row from multiple effects */
EffectRow *effect_row_concrete(Arena *a, Effect **effects, uint8_t n_effects);

/* Create an effect row union */
EffectRow *effect_row_union(Arena *a, EffectRow *left, EffectRow *right);

/* Create an unresolved (symbolic) effect row from an array of symbol names.
 * Uppercase names will resolve to concrete effects; lowercase to row variables.
 * Must be resolved by effect_row_resolve() before the inference pass runs.
 * `span` is the `#fx{...}` form the names were read from (TUR-E0026 points
 * at it); pass a zero Span when there is none. */
EffectRow *effect_row_unresolved(Arena *a, const Symbol **sym_names,
                                 uint8_t n_sym_names, Span span);

/* ET4: Returns true if `child` is the same as `parent_eff` or is a descendant via ^extends */
bool effect_is_subeffect(const Effect *child, const Effect *parent_eff);

/* FH4.1: collect a row's effect names (deduplicated) into `out` (bounded by
 * `cap`); compare two rows as unordered name sets; format a row's names as
 * "A | B" (no braces).  Used by handler-type (TY_HANDLER) operations. */
void effect_row_collect_names(EffectRow *row, const Symbol **out, uint8_t *n, uint8_t cap);
bool effect_row_name_set_eq(EffectRow *a, EffectRow *b);
void effect_row_format_names(Buf *b, EffectRow *row);

/* Check if an effect row is empty */
bool effect_row_is_empty(EffectRow *row);

/* True when `row` names nothing a handler could ever see: it is empty, or
 * every effect in it is a `^capability` tag (an authority annotation that is
 * never performed and never handled -- IO on `println`, FS, Bt).  The CPS and
 * emitter gates that ask "can this function perform an effect at runtime?"
 * use this rather than effect_row_is_empty: a function whose row is {IO}
 * because it prints is as pure, to a handler, as one whose row is {}.  A row
 * variable or an unresolved row is not runtime-pure (unknown). */
bool effect_row_is_runtime_pure(const EffectRow *row);

/* `row` with its `^capability` effects removed (ERK_CONCRETE and ERK_UNION
 * are filtered; other kinds are returned unchanged).  For the lints that ask
 * what a body PERFORMS -- an unreachable handler (W0033), an always-concrete
 * row variable (W0032) -- where a capability tag says nothing. */
EffectRow *effect_row_without_capabilities(Arena *a, EffectRow *row);

/* Check if an effect row contains a specific effect */
bool effect_row_contains(EffectRow *row, Effect *effect);

/* Check if effect row r1 is a subset of r2 (r1 ⊆ r2) */
bool effect_row_is_subset(EffectRow *r1, EffectRow *r2);

/* Create a union of two effect rows */
EffectRow *effect_row_merge(Arena *a, EffectRow *r1, EffectRow *r2);

/* Return a copy of `row` with the named effect removed.
 * Used during effect-row inference to absorb handled effects at EX_HANDLE nodes
 * (effect re-opening: the remaining unhandled effects propagate to the caller).
 * Only ERK_CONCRETE rows are modified; other kinds are returned unchanged. */
EffectRow *effect_row_remove(EffectRow *row, const Symbol *effect_name, Arena *a);

/* Print an effect row for debugging */
void effect_row_print(Buf *b, EffectRow *row);

/* Effect environment - global registry of effects */
typedef struct EffectEnv {
    Effect **effects;        /* All defined effects */
    uint32_t n_effects;
    uint32_t cap_effects;
} EffectEnv;

/* Built-in effect names (v2+). */
#define EFFECT_NAME_UNSAFE "Unsafe"

/* Resolve an ERK_UNRESOLVED row against the populated effect environment.
 * Uppercase symbol names are looked up in env and become ERK_CONCRETE entries;
 * lowercase symbol names become ERK_VAR entries.  An uppercase name env does
 * not declare is left out of the result -- it is the CALLER's job to report
 * it first (effect_row_unknown_names / TUR-E0026), because a dropped name
 * turns `#fx{IO}` into `#fx{}` and silently stops a caller's `#fx{}` being
 * checked.  Non-UNRESOLVED rows are returned unchanged. */
EffectRow *effect_row_resolve(EffectRow *row, EffectEnv *env, Arena *a);

/* TUR-E0026: collect the uppercase names in an ERK_UNRESOLVED row that no
 * effect in `env` declares -- exactly the names effect_row_resolve() would
 * drop.  Writes at most `cap` of them to `out` and returns how many there are
 * in total.  Returns 0 for any row that is not ERK_UNRESOLVED. */
uint8_t effect_row_unknown_names(const EffectRow *row, EffectEnv *env,
                                 const Symbol **out, uint8_t cap);

EffectEnv *effect_env_new(Arena *a);

/* Register a new effect in the environment.
 * defining_module_name: the module that declares this effect (NULL = top-level/stdlib).
 * is_private: true if declared with ^private; only visible within defining_module_name. */
Effect *effect_env_register(EffectEnv *env, Arena *a, const Symbol *name,
                            const Symbol **param_names, TypeKind *param_types,
                            uint8_t n_params, TypeKind result_type,
                            const Symbol *defining_module_name, bool is_private);

/* Look up an effect by name */
/* PS4 (playground-session-hygiene-plan): give an already-registered effect a
 * new declaration in place -- a later REPL/playground turn redefining it.  The
 * Effect keeps its identity (earlier turns' nodes point at it); its signature,
 * visibility, parent and the post-registration Tier C / multishot fields are
 * reset to what a fresh effect_env_register would give, for the caller to fill
 * in again. */
void effect_redefine(Effect *eff, const Symbol **param_names,
                     TypeKind *param_types, uint8_t n_params,
                     TypeKind result_type,
                     const Symbol *defining_module_name, bool is_private);

Effect *effect_env_lookup(EffectEnv *env, const Symbol *name);

/* Check if an effect is valid (exists in environment) */
bool effect_env_contains(EffectEnv *env, const Symbol *name);

/* Register built-in `Unsafe` effect (idempotent). */
Effect *effect_env_register_builtin_unsafe(EffectEnv *env, Arena *a,
                                           const Symbol *unsafe_name);

/* effect-row-honesty-plan W2: the compiler-known capability tags -- IO, and
 * FS / Net / Proc / Rand `^extends IO` -- registered with their parent links
 * and is_capability set, so `#fx{IO}` resolves in a program that loads
 * nothing.  Idempotent.  Every EffectEnv that registers Unsafe registers
 * these too (elab, PASS_EFFECT_LOWER, the interpreter's session env). */
void effect_env_register_builtin_capabilities(EffectEnv *env, Arena *a,
                                              SymbolTable *st);

/* True when `name` is one of those tags; *parent_out (if non-NULL) receives
 * its built-in parent's name, or NULL for IO.  A `defeffect` of such a name
 * is accepted only when it declares exactly the built-in (elab_effects.c). */
bool effect_builtin_capability(const char *name, const char **parent_out);

/* ---------------------------------------------------------------------------
 * Phase P19-4: Effect-row substitution for row-variable unification.
 * An EffectRowSubst maps effect-row variables (Symbol names) to concrete or
 * partially-solved EffectRow values.  Used during effect-row inference to
 * propagate constraints when a polymorphic callee is instantiated.
 * --------------------------------------------------------------------------- */

#define EFFECT_ROW_SUBST_CAP 32

typedef struct {
    const Symbol *var;   /* Row-variable name */
    EffectRow    *row;   /* The row it has been bound to */
} EffectRowSubstEntry;

typedef struct {
    EffectRowSubstEntry entries[EFFECT_ROW_SUBST_CAP];
    uint32_t            n;
} EffectRowSubst;

/* Allocate a fresh, empty substitution on arena `a`. */
EffectRowSubst *effect_row_subst_new(Arena *a);

/* Bind variable `var` to `row` in `subst`.
 * If `var` is already bound, the existing binding is kept (first wins). */
void effect_row_subst_bind(EffectRowSubst *subst,
                           const Symbol *var, EffectRow *row);

/* Look up `var` in `subst`.  Returns NULL if not bound. */
EffectRow *effect_row_subst_lookup(const EffectRowSubst *subst,
                                   const Symbol *var);

/* Unify two effect rows, recording bindings for any row variables in `subst`.
 * Returns true on success, false if the rows are incompatible (e.g. two
 * different concrete effects conflict).  Arena `a` is used for allocations. */
bool effect_row_unify(EffectRow *r1, EffectRow *r2,
                      EffectRowSubst *subst, Arena *a);

/* Return true if `var_name` appears free in `row` (occurs check).
 * Used by effect_row_unify to prevent binding a variable to a row that
 * contains itself, which would produce an infinite effect row. */
bool effect_row_occurs(const Symbol *var_name, EffectRow *row);

/* Apply `subst` to `row`, replacing bound ERK_VAR nodes with their values.
 * Unbound variables are left as ERK_VAR.  Returns a new EffectRow on `a`. */
EffectRow *effect_row_apply_subst(EffectRow *row,
                                  const EffectRowSubst *subst, Arena *a);

#endif /* TUR_EFFECT_H */
