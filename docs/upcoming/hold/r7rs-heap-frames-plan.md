# Plan: Scheme recursion depth bounded by memory, not by the C stack

> **Status:** ON HOLD. Nothing here is scheduled. It records the design so
> the reasoning survives if demand shows up.
> **Type:** Compiler / r7rs runtime
> **Predecessors:** `docs/archive/r7rs-deep-recursion-segfaults-silently.md`
> (PR #1082: `main` runs on a 1 GiB thread, and an overflow prints a message).
> **Lifts the hold:** a real program that needs more than 1 GiB of stack, or a
> `call/cc`-heavy program whose capture cost is dominated by stack depth.

## Why this exists

A compiled `#lang r7rs` program's non-tail calls are C calls, so its
recursion depth is the size of its C stack. PR #1082 made that stack 1 GiB on
64-bit hosts. That is enough for SICP 1.2.1's million-deep linear recursion
(measured through 2e7 frames on Linux), and it is reserved address space, not
memory. It is still a fixed limit tied to the C stack.

Chibi Scheme has no such limit tied to the host. Its VM keeps Scheme frames
on a heap-allocated stack object that it grows when it fills, so the limit is
a configured maximum and the memory the program can get. Racket and Chez
behave similarly in effect. This plan asks what the same property would cost
a compiled Turmeric program.

## What the CPS work does and does not cover

It is tempting to assume the CPS backend already decoupled Turmeric from the C
stack. It did not, for ordinary calls:

- **CPS is per function and only for colored functions.** The CT-IR backend
  (`src/compiler/emit_cps_ir.c`) lowers functions that perform effects, use
  `shift`/`reset`, or `await`
  (`docs/archive/cps-backend-unification-plan.md`). An uncolored function,
  which is nearly all Scheme code, is emitted as direct C.
- **`call/cc` in a compiled r7rs program copies the C stack** between itself
  and the program's start (`docs/guides/r7rs-guide.md`, "Any other `call/cc`
  copies the stack"), so its cost grows with depth.
- **The collector scans the C stack conservatively.** `src/runtime/r7gc.c`
  finds each thread's stack bounds through pthreads and scans the whole live
  range at every collection. Every word on that stack is a possible pointer.
- **The tree-walking interpreter is the exception.** `tur --interpret` keeps
  its frames on the heap, which is why it answered the million-deep case
  before #1082. It pays about 4 KiB per step for it (CLAUDE.md, "Memory, not
  CPU, inside a single turi run").

So "frames on the heap" for compiled code is new work, not a switch to flip.

## A limit stays either way

Removing the C stack limit does not remove the need for a limit. A recursion
that never ends must still stop with a message, not consume all of the
machine's memory and get killed by the OOM killer. Chibi has a configured
maximum stack size for the same reason. Whatever lands here keeps a cap,
configurable, with the overflow message #1082 added. What changes is that the
cap becomes a memory budget rather than an address range fixed at startup.

## Options

### A. Full CPS with a trampoline

Color every r7rs procedure and send it through the CT-IR backend, so each
non-tail call allocates its continuation frame on the heap and returns to a
trampoline.

- For: the machinery exists (DK runtime, `cps-tramp-resume`). `call/cc`
  becomes capturing a pointer, with no stack copy. Depth is bounded by memory.
- Against: a heap allocation per non-tail call, on every call, deep or not.
  `docs/archive/fn-value-call-cps-frames-held-until-outer-entry.md` shows
  CPS frames today are only reclaimed when the outermost direct entry
  returns, so a long-running loop grows. Every call from C into Scheme (a
  `qsort` comparator, a thread start) has to enter a trampoline. Expect a
  large constant-factor slowdown on call-heavy code; it has not been measured.

### B. Cheney on the MTA (Chicken Scheme's design)

CPS-convert, but allocate frames on the C stack and never return. When the
stack reaches a limit, a minor collection copies the live frames to the heap
and `longjmp`s back to the top.

- For: allocation is a pointer bump on the C stack, and `call/cc` is free.
- Against: it needs a **moving** nursery collector. `r7gc.c` is conservative
  and non-moving, and a conservative scan cannot relocate an object that an
  ambiguous word might point at. This means replacing the collector, which is
  far beyond the size of the problem.

### C. Segmented stacks (recommended if this is picked up)

Keep the direct C code. At each r7rs procedure's entry, compare the stack
pointer against the current segment's limit. When it is close, allocate a
new segment (say 1 MiB, `mmap`ed with a guard page), switch to it for the
call, and switch back when the call returns. Depth is then bounded by memory
and the cap.

- For: the cost on the hot path is one compare and a well-predicted branch
  per call. Code that never goes deep never sees a second segment. The
  existing stackful fiber runtime (`src/async/fiber.c`, and
  `src/platform_ucontext_win.h` on Windows) already switches stacks, so the
  switch primitive and its sanitizer annotations exist.
- Against: the "hot split" problem. A call at a segment boundary inside a
  loop allocates and frees a segment every iteration. Go hit this and moved
  to contiguous stacks that are copied when they grow, but copying is not
  available here for the same reason as in B (a conservative scan cannot
  relocate pointers into the stack). The mitigation is to keep one spare
  segment cached per thread so that crossing the boundary repeatedly reuses it.
- Against: every consumer of "the stack is one contiguous range" has to learn
  about the chain of segments. That is the bulk of the work (stages below).

### D. Do nothing past #1082

1 GiB is deeper than any program has needed so far. This is the current
choice, and it is why the plan is on hold.

## Stages for option C

Each stage lands separately, behind `--enable=r7rs-segmented-stack` until it
graduates (CLAUDE.md, "Experimental Compiler Features").

- **H0, measure first.** Add the entry check alone (compare and branch, never
  switching) to every r7rs procedure and measure the r7rs benchmarks and the
  SICP fixtures at `-O0` and `-O2`. Pick a non-inlinable input so clang does
  not fold the work away. If the check costs more than a few percent, stop
  here and record the number.
- **H1, the switch.** Linux x86-64 and arm64, one thread. Allocate, switch,
  return, and cache one spare segment. The entry check leaves headroom (for
  example 64 KiB) so that inline C and libc called from a Scheme frame never
  overflow a segment without passing a check.
- **H2, the collector.** `r7gc.c` scans a thread's segment chain instead of
  one pthread range, including when the thread is stopped by the signal and
  its stack pointer is in a segment other than the one it started on.
- **H3, `call/cc`.** Capturing copies the chain from the capture point to the
  prompt. Re-entering restores it. The "close to an earlier copy" sharing that
  `tur --interpret` does is worth adopting here.
- **H4, threads and platforms.** Every thread gets its own chain. macOS
  (`pthread_get_stackaddr_np` bounds become per-segment bounds) and Windows
  (fibers, or the existing ucontext shim).
- **H5, the cap and the message.** A configurable total (shared with the
  size setting, `TUR_MAIN_STACK_MB` --
  `docs/archive/r7rs-deep-stack-size-not-configurable.md`),
  and the overflow message when the cap is reached, instead of the guard-page
  handler.
- **H6, the JIT.** The entry check is emitted C, so it compiles under c2mir.
  The switch primitive needs to be callable from MIR code; check that before
  H1 commits to an asm-only switch.

## Out of scope

- Typed Turmeric code (`#lang turmeric`). Nobody has asked for unbounded
  recursion there, and it would put a check on every call in every program,
  not only Scheme ones.
- Replacing the collector. If B ever becomes interesting, it is a collector
  plan first.

## Open questions

- How deep do real Scheme programs in the corpus go? H0 should record the
  maximum depth of every r7rs fixture, which tells us whether anything is near
  1 GiB at all.
- Does the entry check interfere with the tail calls that do work today
  (`docs/guides/performance-guide.md`)? A check before a self tail call that
  became a loop is free; one that blocks `musttail` is not.
