/* effect_check.h — Effect-row inference pass (Phase P19-2).
 *
 * This pass runs after elaboration and the EffectEnv pass and before CPS.
 * It walks the elaborated AST, infers the set of effects each `defn` may
 * perform, stores the result in FnDef.inferred_effect_row, and then checks
 * that the inferred row is a subset of the declared row (if present).
 */

#ifndef TUR_EFFECT_CHECK_H
#define TUR_EFFECT_CHECK_H

#include "arena.h"
#include "diag.h"
#include "effect.h"
#include "expr.h"

/* Run the effect-row inference and checking pass over a whole program.
 *
 * For every EX_FN_DEF in `program`:
 *   1. Walk its body collecting all effects that are directly performed
 *      (EX_PERFORM nodes) or indirectly
 *      performed (calls to other defn whose inferred row has been computed).
 *   2. Store the merged row in FnDef.inferred_effect_row.
 *   3. Call effect_row_check_declared() to emit TUR-E0009 if the inferred row
 *      exceeds the declared annotation.
 *
 * `env` is the EffectEnv populated by PASS_EFFECT_ENV.
 * Returns 0 on success, 1 if any diagnostic error was emitted.
 */
int effect_check_pass(Arena *a, Expr *program, EffectEnv *env);

/* sepcomp-cross-module-effect-unhandled: tell the pass the unit is one module
 * of a separately compiled build, so it infers rows for the other modules'
 * callees and refuses a call that would carry a user effect across. */
void effect_check_set_separate_compilation(bool on);

/* Validate a single function definition against its declared effect row.
 * Emits TUR-E0009 for each effect that is inferred but not declared.
 * Does nothing when fd->binding->type.as.fn.effect_row is NULL (unannotated
 * functions are unconstrained in v1).
 * Returns 0 on success, 1 if an error was emitted.
 */
int effect_row_check_declared(FnDef *fd, Arena *a);

/* ER6: --dump-effects — print each top-level defn's inferred effect row to `out`.
 * Format: "fn-name : #{Effect1 Effect2}" (or "#{}" for pure).
 * Called after effect_check_pass completes so inferred rows are available.
 */
void effect_check_dump_effects(Expr *program, FILE *out);

#endif /* TUR_EFFECT_CHECK_H */
