#ifndef TUR_CPS_H
#define TUR_CPS_H

#include "expr.h"
#include "arena.h"
#include "typeclass.h"

/* Phase 18: CPS transformation for delimited continuations */

/* Transform a program to CPS form.
 * Returns the transformed program, or NULL on error.
 * `tc_env` (CPS-CL10): if non-NULL, captured bindings at each cloneable-shift
 * site are checked for a Clone typeclass instance; missing instances are
 * diagnosed as TUR-E0014. */
Expr *cps_transform(Arena *a, Expr *program, TypeClassEnv *tc_env);

/* Check if an expression contains shift or shift0 */
bool cps_expr_contains_shift(const Expr *e);
/* cps-coloring-walk-has-no-arm-for-union-inject: does this expression use a
 * control operator DIRECTLY (call/cc, shift, perform, await, ...), without
 * descending into nested fn bodies?  This is the coloring seed, and the
 * elaborator consults it to decide whether an `any` widen has to hoist its
 * operand into a let -- a widen over a control op is not lowerable by the
 * CPS IR, but a widen over the LET VARIABLE is. */
bool cps_expr_uses_control(const Expr *e);
/* The one enumeration of every evaluated operand of an expression (nested fn
 * definitions -- EX_FN_DEF / EX_FN / EX_CLOSURE -- are boundaries and are NOT
 * visited).  `visit` returns true to stop; returns whether it stopped.  See
 * the header comment at its definition for why it exists. */
bool cps_visit_children(const Expr *e,
                        bool (*visit)(const Expr *, void *),
                        void *ud);
bool cps_expr_contains_effect_op(const Expr *e);

/* Check if a function definition needs (one-shot) CPS transformation */
bool cps_fn_needs_transform(const FnDef *fd);

/* await-through-fn-value-parks-only-the-callee: may calling this function
 * suspend on an `await`?  Its own body awaits, or it calls (by name) a
 * function cps_color_program marked `may_await`.  Nested lambdas and
 * `(async ...)` bodies are not this function's suspension.  NULL -> false. */
bool cps_fn_may_await(const FnDef *fd);
bool cps_fn_installs_handle(const FnDef *fd);

/* Phase B2: Cloneable CPS pass */

/* Check if an expression contains cloneable-shift or cloneable-reset.
 * Functions containing these forms need a cloneable CPS transformation
 * (where the captured environment must be deep-cloneable via Clone). */
bool cps_expr_contains_cloneable_shift(const Expr *e);

/* Check if a function definition needs cloneable CPS transformation.
 * Returns true if the function body contains EX_CLONEABLE_SHIFT or
 * EX_CLONEABLE_RESET anywhere in its call tree. */
bool cps_fn_needs_cloneable_transform(const FnDef *fd);

/* CPS-CL11: For each EX_CLONEABLE_SHIFT in `program`, resolve the per-capture
 * Clone instance method and record its C name into the node's
 * capture_clone_fns[] array (parallel to live_captures[]).  drop fns are
 * recorded in capture_drop_fns[] when a Drop instance exists (currently
 * always NULL until a Drop typeclass is introduced).
 *
 * `tc_env` may be NULL, in which case nothing is recorded (emit.c falls back
 * to bitwise copy + free).  Bindings whose type has no Clone instance get a
 * NULL slot — CPS-CL10 (cps_check_cloneable_captures) will already have
 * emitted TUR-E0014 for those.  This pass is run from cps_transform after
 * liveness and capture-checking, before emit.c. */
void cps_emit_capture_environment(Arena *a, Expr *program, TypeClassEnv *tc_env);

/* CPS1 (cps-transform-plan): whole-program "may-capture" coloring analysis.
 *
 * A function is *colored* (must be CPS'd) iff it can dynamically reach a control
 * operator. cps_color_program builds the call graph over `program`, seeds the
 * colored set with functions whose body directly contains a control-op node
 * (shift, shift0, reset, the cloneable and serial variants, perform, handle,
 * resume, discontinue), conservatively colors any function with an
 * unresolved (indirect) call, and
 * propagates backward to a least fixed point. The result is stored additively in
 * each FnDef's `cps_colored` field; nothing in the existing pipeline is
 * perturbed. See CPS0.1 for the ratified coloring rule.
 *
 * Idempotent: clears and recomputes `cps_colored` on every call. */
void cps_color_program(Arena *a, Expr *program);

/* CPS1.4: --dump-cps-coloring: run cps_color_program (if not already run) and
 * print one line per top-level defn:
 *     cps-coloring: <name> COLORED   (reachable to a control op)
 *     cps-coloring: <name> uncolored (provably control-op-free)
 * Output goes to `out` (stdout under the CLI flag) so a fixture can assert the
 * colored/uncolored partition. Gated behind the dev flag per CPS1.4. */
void cps_dump_coloring(Arena *a, Expr *program, FILE *out);

/* Phase B5: --dump-clone-plan: walk the program and print a summary of every
 * EX_CLONEABLE_SHIFT site — its location and the clone function selected for
 * each captured binding (NULL = bitwise copy fallback). */
void cps_dump_clone_plan(const Expr *program, FILE *out);

/* CPS1: --dump-cps-coloring: walk the top-level FnDef list and print each
 * function with its coloring ("colored" if may_capture, "uncolored" otherwise).
 * Activated by the --dump-cps-coloring CLI flag after cps_transform. */
void cps_dump_cps_coloring(const Expr *program, FILE *out);

#endif
