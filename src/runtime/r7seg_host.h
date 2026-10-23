/* r7seg_host.h -- the segmented stack under `tur jit` (r7rs-heap-frames-plan
 * H6).  `tur` compiles src/runtime/r7seg.c with TUR_SEG_HOST where the
 * segmented stack exists (TUR_SEG_HOST_ON); the engine (src/jit_engine.c)
 * arms its entry thread with it, and a JIT'd program's entry check calls
 * tur_seg_jit_hot / tur_seg_jit_aim and the exported tur_seg_tramp. */
#ifndef TUR_R7SEG_HOST_H
#define TUR_R7SEG_HOST_H

#include <stddef.h>
#include <stdint.h>

#if (defined(__GNUC__) || defined(__clang__)) && defined(__linux__) && \
    (defined(__x86_64__) || defined(__aarch64__))
#define TUR_SEG_HOST_ON 1

/* Layout-identical to r7seg.c's tur_seg_pair. */
typedef struct { unsigned char *lo; uintptr_t span; } tur_seg_jit_pair;

/* The calling thread's pair: the check passes while sp - lo <= span. */
tur_seg_jit_pair *tur_seg_jit_hot(void);
/* The procedure the trampoline is about to re-enter, and its stack bytes. */
void tur_seg_jit_aim(void *fn, size_t nb);
/* Arm the calling thread: `total` is the cap on its stack plus segments
 * (asked: it came from TUR_MAIN_STACK_MB, else half of physical memory). */
void tur_seg_jit_arm(size_t total, int asked);
/* Disarm it and unmap what its chain still holds. */
void tur_seg_jit_end(void);
/* 1 when `a` is in the guard of one of the calling thread's segments. */
int tur_seg_jit_fault_is_overflow(const void *a);
#endif

#endif /* TUR_R7SEG_HOST_H */
