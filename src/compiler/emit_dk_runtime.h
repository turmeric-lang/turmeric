#ifndef TUR_EMIT_DK_RUNTIME_H
#define TUR_EMIT_DK_RUNTIME_H

#include <stdbool.h>
#include "buf.h"

/* =========================================================================
 * emit_dk_runtime.h -- the DK delimited-control RUNTIME prelude emitters
 * (cps-backend-unification U7, step 1: "relocate the runtime").
 *
 * These functions emit the self-contained C runtime that the generated
 * program's delimited-control code links against: the DK multi-prompt
 * machine, the escape-continuation runtime, the cloneable<->DK bridge, and
 * the serial marshaling runtime. They are pure string emitters -- no
 * dependency on the direct emitter's lowering state.
 *
 * They live in a NEUTRAL module (not emit_cps.c) on purpose: BOTH backends
 * depend on this runtime. The direct-style lowering functions in emit_cps.c
 * generate calls into it (dk_run / dk_shift / ...), and so does the native
 * CT-IR backend (emit_cps_ir.c, dk_copy_range / dk_invoke / __sk_frame_for_tag
 * / ...). When U7 eventually deletes emit_cps.c's *lowering* functions, this
 * runtime must survive -- relocating it here decouples the runtime's lifetime
 * from the lowering's, so the delete does not take the load-bearing runtime
 * with it. See docs/archive/cps-backend-unification-u7-readiness-plan.md.
 *
 * The whole-program prelude gates that decide whether each prelude is emitted
 * are the `preamble_uses_*` presence scans local to emit_module.c (D5 relocated
 * them off the deleted emit_cps.c; the cloneable family reuses cps.c's
 * cps_expr_contains_cloneable_shift).
 * ========================================================================= */

/* Emit the self-contained DK multi-prompt machine (a faithful C port of
 * src/runtime/cps_prompt.c) into the generated program's preamble. Call once,
 * gated on preamble_uses_base_delimited().
 *
 * Always includes E7's trampolined tail-resume machinery (struct DK tail_resume
 * field, dk_handler_tail, dk_tail_resume + __dk_drive_after,
 * and dk_perform's yield branch).  This used to be a `tramp` parameter with an
 * `_ex` spelling, defaulting off; cps-tramp-resume graduated 2026-07-19, the
 * only caller passed the always-true g_opt_cps_tramp_resume, and the
 * tramp-off wrapper had no callers at all. */
/* Emit the tur_jmp_buf / TUR_SETJMP / TUR_LONGJMP landing-pad selection.
 * Call ONCE, unconditionally, after <setjmp.h> and before any emitter that
 * declares a landing -- the DK trampoline, the call/cc escape, shift/reset,
 * the handler node, per-fiber panic recovery, cancellation.  Those are gated
 * independently, and several can appear with no delimited control at all, so
 * this cannot live inside any one of their preludes. */
void emit_tur_jmp_buf_prelude(Buf *out);

void emit_cps_runtime_prelude(Buf *out);

/* True while emit_rt_split_source() is producing the CANONICAL split-runtime
 * text (S2): the committed src/runtime/generated/tur_rt_split*.{c,h}, and the
 * probe cmd_jit hashes against them.  Defined in emit_module.c, where the flag
 * it reports lives.
 *
 * The DK prelude needs it for exactly one decision -- which setjmp/longjmp pair
 * backs the tail-resume landing on Windows -- because that choice cannot be made
 * by the preprocessor there.  Under S2 the runtime half is compiled by the host
 * toolchain (GCC) while the program half goes through c2mir, so a `#if
 * defined(__GNUC__)` resolves DIFFERENTLY in the two halves of one program: the
 * host arms a landing with one mechanism and the program half arms it with the
 * other, and since setjmp/longjmp must PAIR, the mismatch is silent -- the
 * program runs to exit 0 having produced nothing.  Deciding here instead makes
 * both halves agree by construction, because they are emitted from this same
 * text. */
bool rt_split_canonical_emission(void);

/* Emit the undelimited escape-continuation runtime (tur_escape_cont +
 * tur_escape_resume) into the generated preamble. Call once, gated on
 * preamble_uses_callcc(). */
void emit_cps_callcc_prelude(Buf *out);

/* Emit the cloneable-continuation <-> DK bridge (__dk_cont_fn /
 * __dk_env_clone / __dk_env_drop). Call once, after both the cloneable
 * runtime and the DK machine prelude, gated on
 * cps_expr_contains_cloneable_shift(). */
void emit_cps_cloneable_bridge_prelude(Buf *out);

/* Emit the serial marshaling runtime (fixed tagged context frames +
 * tur_serial_cont_resume / _serialize / _deserialize). Call once, after the DK
 * machine prelude, gated on preamble_uses_serial(). */
void emit_cps_serial_runtime_prelude(Buf *out);

#endif
