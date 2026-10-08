#ifndef TUR_EMIT_CPS_IR_H
#define TUR_EMIT_CPS_IR_H

#include <stdbool.h>

#include "emit_internal.h"

/* =========================================================================
 * CPS-IR-to-C backend (cps-ir-to-c-backend-plan, Phase C1).
 *
 * Lowers a colored function's ANF/CPS IR (a CTerm, src/passes/cps_ir.h) to C
 * under the DK-threading ABI ratified in Phase C0:
 *
 *   colored `f a b`  ->  int64_t f__cps(int64_t a, int64_t b, DK *k)
 *
 * plus a direct-style entry wrapper `f(a, b)` that seeds a dk_done()-terminated
 * root continuation so uncolored/direct callers reach the CPS body unchanged.
 *
 * Fallback-guarded and whole-function granular (plan "Scope and guiding
 * constraint"): a colored function is emitted here ONLY when its entire CTerm
 * lies in the C1 emittable subset -- the non-delimited control/data core
 * (CT_APPCONT, CT_LETVAL, CT_LETPRIM, CT_LETCALL, CT_TAILCALL, CT_LETCONT,
 * CT_IF), scalar (int/bool) types, and no join point that must be reified onto
 * the heap chain (a non-tail cps->cps call).  Everything else keeps its
 * existing direct-style emission.  Coverage grows monotonically as later phases
 * close the gaps.
 *
 * The backend is always-on (graduated 2026-07-11): a colored function in the
 * emittable subset is CPS-emitted here, and everything else falls back to the
 * direct emitter.
 * ========================================================================= */

/* True if `program` has at least one colored function the C1 backend will emit.
 * Self-contained (colors the program and computes the emittable set with its
 * own arena); used to gate the DK runtime prelude.  Returns false when no
 * colored function lands in the emittable subset. */
bool emit_cps_ir_program_has_emittable(const Expr *program);

/* Whether the CT-IR CPS backend emits `b`'s function as `<b>__cps(..., DK*)`.
 * The legacy CPS3 `--cps-path` wrapper path uses this to skip a colored function
 * the cps-backend already emits (else the two declare the same `__cps` symbol
 * with different signatures -- a `conflicting types` error). */
bool emit_cps_ir_emits_binding(const Expr *program, const Binding *b);

/* cps-effectful-closure-returned-through-empty-row-aborts: the body of an
 * unthreaded fn-value (a lambda or a named fn used as a value, whose calls
 * start with no handler in scope) that lets `eff` escape, directly or through
 * a colored fn it calls -- the reason that effect's performers left the CPS
 * backend.  NULL when none does.  Valid after the program was classified. */
const Expr *emit_cps_ir_effect_escaping_fnval(const Symbol *eff);
/* SR2b: colored generic whose base signature sig-rejects -- needs a monomorph
 * clone (the G3a island path's precondition).  See the definition. */
bool emit_cps_ir_colored_fn_needs_mono(const struct FnDef *fd);

/* If `fn_def_expr`'s FnDef is colored and lies in the C1 emittable subset, emit
 * its CPS body + direct-entry wrapper into `file` and return true.  Otherwise
 * emit nothing and return false, so the caller falls back to the direct-style
 * path (emit_fn_def).  Returns false (a no-op) for any function outside the
 * emittable subset. */
bool emit_cps_ir_try_fn(EmitCtx *ctx, Buf *file, const Expr *fn_def_expr);

/* Forget everything cached for the last program: the classified CTerms and
 * their arena, and fd_for_binding's table.
 *
 * Those caches are keyed on the program's ADDRESS (and the EmitCtx's).  A
 * process that compiles more than once -- `tur mcp`, `tur lsp`, an embedder --
 * frees both between compiles, and the allocator is free to hand the next
 * program the very same addresses.  The key then matches, and the stale
 * CTerms, whose Bindings were freed with the old program, get emitted: an
 * access violation in raw_name_for_binding after a few dozen MCP requests
 * (docs/archive/mcp-server-exits-mid-session-on-windows.md).  The emission
 * entry points (emit_module.c) call this at the start and end of every
 * top-level emission. */
void emit_cps_ir_forget(void);

/* cps-self-tail-call-relies-on-sibling-call: write the fused function of any
 * CPS mutual tail-call group not yet defined in this translation unit. */
void emit_cps_ir_flush_groups(Buf *file);

#endif
