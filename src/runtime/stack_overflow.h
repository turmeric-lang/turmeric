/* stack_overflow.h -- what counts as a stack overflow, and what it says.
 *
 * One definition shared by the two places that run a program's `main` on a
 * stack they sized themselves and so know the bounds of:
 *
 *   - a compiled `#lang r7rs` program's deep stack (emit_deep_stack_runtime,
 *     src/compiler/emit_module.c), which pastes both into the program's C as
 *     text through TUR_STACK_OVERFLOW_STR;
 *   - the JIT's entry thread (src/jit_engine.c), which uses them directly --
 *     under `tur jit` the process that dies is `tur` itself, so the handler
 *     lives in the engine, not in the program's text
 *     (docs/archive/jit-stack-overflow-has-no-message.md).
 *
 * Header-only and dependency-free on purpose: it is read by the host build
 * and, as text, by every emitted r7rs program. */
#ifndef TUR_STACK_OVERFLOW_H
#define TUR_STACK_OVERFLOW_H

/* The line printed (with a newline) before the fault takes the process down. */
#define TUR_STACK_OVERFLOW_MSG "stack overflow: recursion too deep"

/* A SIGSEGV/SIGBUS at `a` (an unsigned char *) is an overflow of a stack whose
 * lowest usable byte is `lo` when it lands within 64 KiB above lo -- a frame
 * that straddles the end -- or within 1 MiB below it: the guard page, or a
 * large frame that jumped it.  `lo` is NULL when the bounds are unknown, and
 * then nothing is an overflow. */
#define TUR_STACK_FAULT_IS_OVERFLOW(a, lo) \
    ((lo) && (a) < (lo) + 65536 && (a) + (1 << 20) >= (lo))

/* The same, as C source text for the emitter: TUR_STACK_OVERFLOW_STR(
 * TUR_STACK_FAULT_IS_OVERFLOW(a, lo)) is the expanded expression in quotes. */
#define TUR_STACK_OVERFLOW_STR_(...) #__VA_ARGS__
#define TUR_STACK_OVERFLOW_STR(...) TUR_STACK_OVERFLOW_STR_(__VA_ARGS__)

#endif /* TUR_STACK_OVERFLOW_H */
