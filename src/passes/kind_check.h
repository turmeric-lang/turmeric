/* kind_check.h — Kind inference and validation pass (Phase HKT H1/H2).
 *
 * This pass runs after elaboration and before effect-lowering.
 *
 * In Phase H1, the pass performs:
 *   1. Kind annotation propagation for typeclass type parameters.
 *   2. Belt-and-suspenders kind validation for definstance type arguments
 *      (the same check performed by elab_definstance, here as a safety net).
 *
 * Phase H2 adds:
 *   3. Bottom-up kind inference: infers the kind of typeclass parameters from
 *      the kinds of the type arguments provided in definstance declarations.
 *      This makes explicit `^f` annotations optional.
 *   4. KindEnv — a symbol → Kind mapping used to propagate inferred kinds.
 *
 * Pipeline position:
 *   PASS_ELABORATE → PASS_KIND_CHECK → PASS_EFFECT_ENV → ...
 */

#ifndef TUR_KIND_CHECK_H
#define TUR_KIND_CHECK_H

#include <stdio.h>
#include "arena.h"
#include "expr.h"
#include "types.h"

/* ---------------------------------------------------------------------------
 * Phase HKT H2: KindEnv — symbol → Kind mapping
 * ------------------------------------------------------------------------- */

/* Opaque kind environment.  Backed by the arena; no explicit free needed. */
typedef struct KindEnv KindEnv;

/* Allocate a new, empty KindEnv backed by arena 'a'. */
KindEnv *kind_env_new(Arena *a);

/* Bind symbol 'sym' to kind 'k' in 'env'.  If 'sym' is already bound its kind
 * is updated to the result of kind_unify(old, k).  'span' is used for any
 * diagnostic emitted during unification. */
void kind_env_bind(KindEnv *env, const Symbol *sym, Kind k, Span span);

/* Look up the kind of 'sym'.  Returns KIND_STAR if 'sym' is not bound. */
Kind kind_env_lookup(const KindEnv *env, const Symbol *sym);

/* ---------------------------------------------------------------------------
 * Phase HKT H2: kind_unify
 * ------------------------------------------------------------------------- */

/* Unify two kinds.
 *
 * Rules (v1 ground kinds only — no kind variables):
 *   unify(k, k)          → k
 *   unify(KIND_STAR, KIND_ARROW) → emit TUR_E0012_KIND_MISMATCH; return KIND_STAR
 *   unify(KIND_ARROW, KIND_STAR) → same
 *
 * 'span' is used for any diagnostic.  Returns the unified kind (or KIND_STAR
 * on error).
 */
Kind kind_unify(Kind k1, Kind k2, Span span);

/* Run the kind-check pass over the elaborated program.
 *
 * Returns 0 on success, 1 if any diagnostic error was emitted.
 */
int kind_check_pass(Arena *a, Expr *program);

/* Phase HKT-P1: Infer the result kind of a type application (type-app fn arg).
 *
 * fn_type must have hkt_kind == KIND_ARROW or KIND_ARROW2:
 *   KIND_ARROW  (fn : * -> *)   applied to one arg → result kind KIND_STAR
 *   KIND_ARROW2 (fn : * -> * -> *) applied to one arg → result kind KIND_ARROW
 *
 * Emits TUR_E0012_KIND_MISMATCH and returns KIND_STAR if fn_type has KIND_STAR
 * (cannot apply a concrete type).
 *
 * 'span' is used for diagnostics.
 */
Kind kind_of_type_app(Type fn_type, Type arg_type, Span span);

/* Phase HKT-P6: Walk the elaborated AST and print, to `out`, every
 * defclass / definstance / defn that carries a non-KIND_STAR kind
 * annotation.  Useful for debugging the kind-inference pipeline with
 * the --dump-kinds flag. */
void kind_dump_program(Expr *program, FILE *out);

/* Phase HKT-P6: Verify that Kind information is preserved through all compiler
 * passes. Returns true if all kind info appears valid, false otherwise.
 * Used in debug builds to catch accidental clearing of kind annotations. */
bool kind_verify_program(const Expr *program);

#endif /* TUR_KIND_CHECK_H */
