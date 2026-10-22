/* tur_tls.c -- thread-local runtime state for front ends without TLS.
 *
 * jit-engine-plan / findings 14.3 and 15.  Multi-threading under the JIT is a
 * requirement (owner decision, 2026-07-29), and c2mir has no thread-local
 * storage at all: it parses `_Thread_local`, warns "Thread local is not
 * implemented", and then treats the variable as an ordinary global.  Under
 * that reading every spawned thread shares one slot -- which is how 8 STM
 * workers ended up sharing one transaction descriptor and losing updates
 * (stm-stress), and why gc-registry-growth SIGSEGVed.
 *
 * The emitted preamble declares a set of thread-local variables.  Under a
 * GNU-family compiler they stay exactly what they were -- plain
 * `TUR_THREAD_LOCAL` file-scope variables, zero indirection.  Under any other
 * front end the emitter #defines each NAME to a deref of one of these
 * accessors (emit_rt_tls, src/compiler/emit_module.c): the host runtime is
 * compiled by a real cc, so `__thread` here is genuine per-thread storage, and
 * each accessor returns the calling thread's instance by address.  Same
 * host-residency pattern as tur_atomics.c, applied to state instead of
 * operations.
 *
 * Slots are deliberately typed void* / int / bool / int64_t / jmp_buf: the pointee
 * types (STM_Transaction, FiberBlock, TurThreadState, ...) are preamble-
 * private structs this TU has no business knowing.  The emitted macro casts
 * the slot back to the precise type at every use site.  All initializers in
 * the preamble are zero, and `__thread` storage zero-initializes per new
 * thread, so the semantics match.
 *
 * jmp_buf is the one non-scalar: its layout is fixed by libc's <setjmp.h>,
 * which both this TU and the emitted program include, so handing the buffer
 * across the boundary is well-defined.
 *
 * The storage itself has external linkage, for the split preamble's hosted cc
 * build: a real compiler can name the thread-local directly, and the decls
 * half does (performance-audit R-2), as `(*(T *)<the accessor's return
 * expression>)` -- an accessor call around every panic-flag check was ~25% of
 * such a program's cycles.  tools/gen-runtime-split.py reads this file to
 * build those reads, so keep each slot to the two-line shape below: a
 * `__thread` definition, then an accessor whose body is one `return`.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>

__thread void *tur_tls_stm_current_tx = 0;
void **tur_tls_stm_current_tx_ptr (void) { return &tur_tls_stm_current_tx; }

__thread void *tur_tls_handler_chain = 0;
void **tur_tls_handler_chain_ptr (void) { return &tur_tls_handler_chain; }

__thread int tur_tls_panicking = 0;
int *tur_tls_panicking_ptr (void) { return &tur_tls_panicking; }

__thread int tur_tls_root_unwind = 0;
int *tur_tls_root_unwind_ptr (void) { return &tur_tls_root_unwind; }

__thread void *tur_tls_cur_shift_reset = 0;
void **tur_tls_cur_shift_reset_ptr (void) { return &tur_tls_cur_shift_reset; }

__thread void *tur_tls_current_fiber = 0;
void **tur_tls_current_fiber_ptr (void) { return &tur_tls_current_fiber; }

__thread bool tur_tls_fiber_cancelled_flag = false;
bool *tur_tls_fiber_cancelled_flag_ptr (void) { return &tur_tls_fiber_cancelled_flag; }

__thread void *tur_tls_current_thread_state = 0;
void **tur_tls_current_thread_state_ptr (void) { return &tur_tls_current_thread_state; }

__thread int tur_tls_async_direct_body = 0;
int *tur_tls_async_direct_body_ptr (void) { return &tur_tls_async_direct_body; }

__thread int tur_tls_async_owns_env = 0;
int *tur_tls_async_owns_env_ptr (void) { return &tur_tls_async_owns_env; }

__thread jmp_buf tur_tls_cancel_jmpbuf;
jmp_buf *tur_tls_cancel_jmpbuf_ptr (void) { return &tur_tls_cancel_jmpbuf; }

__thread int tur_tls_cancel_jmpbuf_valid = 0;
int *tur_tls_cancel_jmpbuf_valid_ptr (void) { return &tur_tls_cancel_jmpbuf_valid; }

__thread void *tur_tls_current_scheduler_mt = 0;
void **tur_tls_current_scheduler_mt_ptr (void) { return &tur_tls_current_scheduler_mt; }

__thread int64_t tur_tls_rtv = 0;
int64_t *tur_tls_rtv_ptr (void) { return &tur_tls_rtv; }

/* The DK escape-continuation registry (emit_dk_runtime.c): the prompts live
 * on THIS thread's stack.  Shared, two threads' call/cc prompts truncated and
 * realloc'd one array between them
 * (docs/archive/jit-threaded-program-hangs-under-load.md). */
__thread void *tur_tls_escape_live = 0;
void **tur_tls_escape_live_ptr (void) { return &tur_tls_escape_live; }
__thread int tur_tls_escape_live_n = 0;
int *tur_tls_escape_live_n_ptr (void) { return &tur_tls_escape_live_n; }
__thread int tur_tls_escape_live_cap = 0;
int *tur_tls_escape_live_cap_ptr (void) { return &tur_tls_escape_live_cap; }

/* The r7rs prelude's dynamic environment (emit_dk_runtime.c, tur_r7rs_dyn):
 * the wind, handler and parameter stacks and a re-entry's delivered value,
 * four 16-byte tagged words.  Shared, one thread's raise ran another thread's
 * handler (docs/archive/r7rs-dynamic-environment-shared-across-threads.md). */
__thread _Alignas(16) unsigned char tur_tls_r7rs_dyn[64];
void **tur_tls_r7rs_dyn_ptr (void) { return (void **) tur_tls_r7rs_dyn; }

/* The r7rs prelude's stack bases for a call/cc image (stdlib/r7rs/prelude.tur):
 * the calling thread's own stack top, and the current top-level form's frame.
 * Shared, a worker's capture measured its image against another thread's
 * stack. */
__thread void *tur_tls_r7k_base = 0;
void **tur_tls_r7k_base_ptr (void) { return &tur_tls_r7k_base; }
__thread void *tur_tls_r7k_form_base = 0;
void **tur_tls_r7k_form_base_ptr (void) { return &tur_tls_r7k_form_base; }

/* The DK runtime's per-thread state (emit_dk_runtime.c): the reap registry,
 * the CPS entry depth, and the trampoline's landing, resume chain and
 * value.  Shared, CPS entries on two threads reallocated one
 * registry between them, a worker's exit freed the other threads' live
 * chains, and a worker's entry replaced the landing another thread's
 * tail-resume jumped to (docs/archive/dk-reap-list-shared-across-threads.md). */
__thread void *tur_tls_dk_reap_v = 0;
void **tur_tls_dk_reap_v_ptr (void) { return &tur_tls_dk_reap_v; }
__thread void *tur_tls_dk_reap_kind = 0;
void **tur_tls_dk_reap_kind_ptr (void) { return &tur_tls_dk_reap_kind; }
__thread size_t tur_tls_dk_reap_n = 0;
size_t *tur_tls_dk_reap_n_ptr (void) { return &tur_tls_dk_reap_n; }
__thread size_t tur_tls_dk_reap_cap = 0;
size_t *tur_tls_dk_reap_cap_ptr (void) { return &tur_tls_dk_reap_cap; }
__thread int tur_tls_dk_entry_depth = 0;
int *tur_tls_dk_entry_depth_ptr (void) { return &tur_tls_dk_entry_depth; }
__thread void *tur_tls_dk_driver = 0;
void **tur_tls_dk_driver_ptr (void) { return &tur_tls_dk_driver; }
__thread void *tur_tls_dk_resume_chain = 0;
void **tur_tls_dk_resume_chain_ptr (void) { return &tur_tls_dk_resume_chain; }
__thread intptr_t tur_tls_dk_resume_val = 0;
intptr_t *tur_tls_dk_resume_val_ptr (void) { return &tur_tls_dk_resume_val; }
__thread void *tur_tls_dk_pend_perform = 0;
void **tur_tls_dk_pend_perform_ptr (void) { return &tur_tls_dk_pend_perform; }

/* The dynamic tail-call trampoline (emit_module.c, proper-tail-calls T6): its
 * descriptor, the function it has armed, the driver's root and the sentinel
 * box.  Shared, every thread's tail calls bounced through one descriptor.
 * The descriptor and the box are preamble-private types, so they are raw,
 * aligned bytes here; the emitted side checks that its types fit. */
__thread _Alignas(16) unsigned char tur_tls_tb_desc[256];
void **tur_tls_tb_desc_ptr (void) { return (void **) tur_tls_tb_desc; }
__thread void *tur_tls_tb_armed_for = 0;
void **tur_tls_tb_armed_for_ptr (void) { return &tur_tls_tb_armed_for; }
__thread void *tur_tls_tb_root = 0;
void **tur_tls_tb_root_ptr (void) { return &tur_tls_tb_root; }
__thread _Alignas(16) unsigned char tur_tls_tb_sentinel_box[16];
void **tur_tls_tb_sentinel_box_ptr (void) { return (void **) tur_tls_tb_sentinel_box; }

/* The current panic site (emit_module.c, panic-location-names-the-runtime-
 * not-the-call-site): the source position of the direct call to a panicking
 * inline-C body this thread is making, set just before and cleared just
 * after.  A pointer to a preamble-private static {file, line} pair. */
__thread void *tur_tls_cur_site = 0;
void **tur_tls_cur_site_ptr (void) { return &tur_tls_cur_site; }

#ifdef _WIN32
#include <windows.h>
/* The calling thread's stack base, for the r7rs prelude's call/cc image under
 * `tur jit`.  A GCC-compiled program reads the TEB's NT_TIB.StackBase
 * directly (`movq %gs:0x08`); c2mir has no inline asm, so without this the
 * prelude found no base on the Windows JIT and call/cc could only escape. */
void *tur_win_stack_base (void) {
  return ((NT_TIB *) NtCurrentTeb ())->StackBase;
}
#endif
