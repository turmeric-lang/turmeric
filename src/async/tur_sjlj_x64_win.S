/* x86-64 Windows non-local jump for the Turmeric DK trampoline.
 *
 * WHY THIS EXISTS
 *
 * `longjmp` on win64 is a genuine SEH unwind (`_setjmpex` + `RtlUnwindEx`), and
 * RtlUnwindEx validates every frame's RSP against the *thread's* stack bounds
 * as recorded in the TEB.  A fiber stack is malloc'd, so the TEB knows nothing
 * about it, every frame on it is out of bounds, and the unwind raises
 * STATUS_BAD_STACK (0xc0000028) -- killing the process before any output.  A
 * setjmp/longjmp pair BOTH on the fiber stack fails identically, so this is not
 * a cross-stack mistake that bookkeeping could avoid.
 *
 * The cc path solves this with GCC's __builtin_setjmp/__builtin_longjmp, which
 * are a plain SP/FP/PC save-restore with no unwinder.  The JIT path cannot:
 * under the S2 split the program half is compiled by c2mir, which has no such
 * builtin and does not define __GNUC__, and setjmp/longjmp must PAIR -- so the
 * split was pinned to plain setjmp on both sides and effects-on-fibers stayed
 * broken there (docs/archive/jit-windows-support-spike.md).
 *
 * These two functions are the way out: an ordinary extern call that BOTH halves
 * can make.  The host-compiled runtime half links this object directly; the
 * c2mir-compiled program half resolves the symbols through jit_engine.c's
 * JIT_SHIMS table.  Nothing has to agree about builtins, only about a symbol
 * name and a buffer size.
 *
 * WHAT IT DOES
 *
 * `tur_sjlj_set` saves the resume point and every callee-saved register the
 * win64 ABI names, and returns 0.  `tur_sjlj_jump` restores them and resumes at
 * that point with the return value 1.  Frames in between are abandoned without
 * running their epilogues, which is why the callee-saved set must be restored
 * from the buffer rather than left to unwinding.
 *
 * This is the save half and the restore half of fiber_ctx_x64_win.S's
 * `tur_ctx_swap`, split apart, and the buffer layout is deliberately the same
 * as `tur_ctx_t` so the two can be read against each other:
 *   0 rip  8 rsp  16 rbx  24 rbp  32 rsi  40 rdi
 *   48 r12  56 r13  64 r14  72 r15  80..240 xmm6..xmm15
 * 240 bytes; keep TUR_SJLJ_WORDS in tur_sjlj.h in step with it.
 *
 * Like `tur_ctx_swap`, this does NOT save MXCSR or the x87 control word.  Both
 * are callee-saved by the ABI, so a jump past a frame that changed the rounding
 * mode and never restored it would leak that mode.  Turmeric does not change
 * either, and the fiber switch has always had the same property.
 */

    .text

/* int tur_sjlj_set(void *buf);   buf = RCX.  Returns 0 here, 1 via _jump. */
    .globl tur_sjlj_set
    .def   tur_sjlj_set; .scl 2; .type 32; .endef
tur_sjlj_set:
    /* Resume point: the return address on the stack becomes rip, and rsp is
     * captured as it will be *after* this function returns. */
    mov    (%rsp), %rax
    mov    %rax,   0(%rcx)
    lea    8(%rsp), %rax
    mov    %rax,   8(%rcx)
    mov    %rbx,  16(%rcx)
    mov    %rbp,  24(%rcx)
    mov    %rsi,  32(%rcx)
    mov    %rdi,  40(%rcx)
    mov    %r12,  48(%rcx)
    mov    %r13,  56(%rcx)
    mov    %r14,  64(%rcx)
    mov    %r15,  72(%rcx)
    movups %xmm6,  80(%rcx)
    movups %xmm7,  96(%rcx)
    movups %xmm8, 112(%rcx)
    movups %xmm9, 128(%rcx)
    movups %xmm10,144(%rcx)
    movups %xmm11,160(%rcx)
    movups %xmm12,176(%rcx)
    movups %xmm13,192(%rcx)
    movups %xmm14,208(%rcx)
    movups %xmm15,224(%rcx)
    xor    %eax, %eax
    ret

/* void tur_sjlj_jump(void *buf);  buf = RCX.  Does not return. */
    .globl tur_sjlj_jump
    .def   tur_sjlj_jump; .scl 2; .type 32; .endef
tur_sjlj_jump:
    /* rcx is caller-saved and nothing below writes it, so it stays valid as the
     * buffer pointer right up to the final indirect jump. */
    movups 224(%rcx), %xmm15
    movups 208(%rcx), %xmm14
    movups 192(%rcx), %xmm13
    movups 176(%rcx), %xmm12
    movups 160(%rcx), %xmm11
    movups 144(%rcx), %xmm10
    movups 128(%rcx), %xmm9
    movups 112(%rcx), %xmm8
    movups  96(%rcx), %xmm7
    movups  80(%rcx), %xmm6
    mov    72(%rcx), %r15
    mov    64(%rcx), %r14
    mov    56(%rcx), %r13
    mov    48(%rcx), %r12
    mov    40(%rcx), %rdi
    mov    32(%rcx), %rsi
    mov    24(%rcx), %rbp
    mov    16(%rcx), %rbx
    mov     8(%rcx), %rsp
    mov    $1, %eax
    jmp    *0(%rcx)
