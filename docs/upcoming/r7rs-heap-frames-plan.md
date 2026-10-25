# Plan: Scheme recursion depth bounded by memory, not by the C stack

> **Status:** GRADUATED 2026-10-25 (prototype 2026-10-23, beta 2026-10-24,
> owner's ruling to graduate 2026-10-24). Every compiled `#lang r7rs` program on
> Linux x86-64 and arm64 carries the check and grows onto segments; on every
> other host -- macOS, Windows, `tur jit`, 32-bit -- and in a build under a
> stack-tracking sanitizer, the check compiles to nothing and the 1 GiB stack
> stays ([Platforms](#platforms-the-graduated-contract)).
> `TUR_R7RS_SEGMENTED_STACK=0` is the bisection hatch, and
> `--enable=r7rs-segmented-stack` is a TUR-W0063 no-op until 0.72.0. The plan
> stays in `docs/upcoming/` for the work after graduation:
> [Left, in order](#left-in-order) (macOS and Windows segments, sanitizer
> support, H6, a fiber's stack that grows).
> **Type:** Compiler / r7rs runtime
> **Predecessors:** `docs/archive/r7rs-deep-recursion-segfaults-silently.md`
> (PR #1082: `main` runs on a 1 GiB thread, and an overflow prints a message).
> **Was on hold until:** a real program needs more than 1 GiB of stack, or a
> `call/cc`-heavy program's capture cost is dominated by stack depth. It was
> picked up on request. Graduation waited on one of those until the owner
> lifted the condition on 2026-10-24 (see [Graduation](#graduation-done-2026-10-25)).

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
  switch primitive exists. Its sanitizer annotations are TSan's only: the
  emitted fiber runtime announces each switch (`TUR_TSAN_FIBER_*`,
  `emit_module.c`), and nothing anywhere tells ASan about a switch -- see
  [H7](#h7-sanitizers-what-keeping-segments-would-take).
- Against: the "hot split" problem. A call at a segment boundary inside a
  loop allocates and frees a segment every iteration. Go hit this and moved
  to contiguous stacks that are copied when they grow, but copying is not
  available here for the same reason as in B (a conservative scan cannot
  relocate pointers into the stack). The mitigation is to keep one spare
  segment cached per thread so that crossing the boundary repeatedly reuses it.
- Against: every consumer of "the stack is one contiguous range" has to learn
  about the chain of segments. That is the bulk of the work (stages below).

### D. Do nothing past #1082

1 GiB is deeper than any program has needed so far (H0 measured the r7rs
fixtures: none needs more than 32 MiB). This was the choice until C was
prototyped; it is still the default, since C is behind a flag.

## Stages for option C

Each stage lands separately, behind `--enable=r7rs-segmented-stack` until it
graduates (CLAUDE.md, "Experimental Compiler Features"). The stages as first
planned; [Progress](#progress-2026-10-24) says where each stands.

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

## Progress (2026-10-24)

| Stage | State |
| --- | --- |
| H0, measure | Done (2026-10-23). Numbers below; the check costs "a few percent" or less once it is placed with care. |
| H1, the switch | Done, Linux x86-64 and arm64. arm64 RAN on 2026-10-24: the whole of `tests/check-r7rs-segmented-stack.sh` as aarch64 binaries under qemu-aarch64 8.2 (`TUR_SEG_CROSS=aarch64`, gcc 13 cross), and the re-entry, thread and fiber programs built by clang 18 for aarch64 too. |
| H2, the collector | Done. `r7gc.c` walks the segment chain (`tur_gc_scan_segments`), every thread's. |
| H3, `call/cc` | Done (2026-10-24). A capture on a segment copies the chain, one piece per stack, and pins its segments; a re-entry from anywhere puts the chain back. Escape-only in one case: a capture inside `(scheme eval)`'s evaluator. |
| H4, threads and platforms | Done on Linux (2026-10-24): every thread the program starts is armed, with its own chain; a fiber's stack is checked against its own bounds (no growth on a fiber). macOS and Windows: no check, by design for graduation ([Platforms](#platforms-the-graduated-contract)); segments there are deferred. |
| H5, the cap | Done. `TUR_MAIN_STACK_MB` is the total, per thread; unset, half of physical memory. |
| H6, the JIT | Not done, deliberately: under `tur jit` (`__MIRC__`) the check compiles to nothing and the engine's own `TUR_MAIN_STACK_MB`-sized thread stays. Why: [H6](#h6-why-the-jit-has-no-check). Deferred past graduation. |

### What landed (2026-10-24): H3, H4, the hatch

- **H4, threads.** From the end of the deep-stack runtime on, `pthread_create`
  names `tur_seg_pthread_create` (`emit_seg_stack_redirect`), so every thread
  the program starts -- SRFI 18's, the scheduler's and async's workers, inline
  C's -- runs `tur_seg_thread_main` first and arms itself
  (`tur_seg_thread_start`): its own stack is the first record of a chain that
  is all thread-local (the base record, spare, pool and budget included), the
  collector is told where it lives, and a thread with no alternate signal
  stack gets one, so a guard-page fault on its segments prints the overflow
  message. The thread unmaps everything when its start routine returns. The
  start routine and its argument cross in a record on the creator's stack,
  which waits until the thread has taken them: a record on the heap was
  collected under torture (the collector stops treating a running thread's
  argument as a root before the start routine has read it), which
  `run-r7rs-gc.sh`'s thread cases caught at about one run in three.
- **H4, fibers.** One swap in the fiber switch's registry (`TurDynSwap`,
  `emit_seg_fiber_swap`): on a resume the fiber's pair and bounds go in -- its
  own stack, with a headroom of an eighth of it up to 256 KiB -- and on the
  switch out the thread's come back. A fiber used to disarm the thread's check
  for the rest of the run (its first check failed on a stack no record held).
  A fiber's stack does NOT grow: the collector finds a suspended fiber's
  frames through its heap-allocated stack, and nothing would find them on a
  segment. So at a fiber's limit the check stops the program with `stack
  overflow: recursion too deep`, where without the flag the fiber runs off
  the end of its `malloc`'d stack into the heap with no message.
- **H3.** `tur_seg_pieces` describes a capture on a segment as one piece per
  stack of the chain -- each segment from the capture (or from where the
  frames resume) to its top, the thread's own stack up to the form's prompt
  or the stack base -- with each segment's link as it was, and pins the
  segments: a pinned segment is never unmapped while the thread lives, only
  parked in the thread's pool when it leaves the chain, because a re-entry
  copies frames back to the same addresses. The prelude's capture copies the
  pieces into one image. Every re-entry on an armed thread now goes through
  `tur_seg_restore`, the contiguous image too: it blocks signals (so the
  collector never stops the thread with the chain half rebuilt), moves the
  stack pointer just below the image's lowest frame (`tur_seg_run_at`, a few
  instructions of asm per architecture), releases the chain it leaves, takes
  the image's segments out of the pool, copies every piece back, relinks the
  chain, and longjmps in. The old re-entry from a segment copied the image in
  place and jumped from wherever it was. Escape-only still: a capture whose
  form prompt is on a segment (no program does that: forms run from `main`)
  and one inside `(scheme eval)`'s evaluator, whose restore hook is handed one
  contiguous image.
- **The hatch, `TUR_R7RS_SEGMENTED_STACK=0`.** In the compiler's environment
  it means no check and none of the runtime (`emit_seg_stack_wanted`): the C
  is byte-identical to a build without the flag. In a program's environment
  it means no thread arms: every check passes, the deep-stack thread is the
  size it is without the flag, `call/cc` takes the prelude's own paths, and
  no thread or fiber hook runs.
- **Tests.** `tests/check-r7rs-segmented-stack.sh` grew re-entry eleven times
  into a 1e6-deep chain (from the top and from deep in a recursion that reuses
  its segments, plainly and under collector torture), four threads 1e6 deep at
  once with a re-entry on a worker (plainly, under torture, and without the
  flag, where it must die), a fiber's recursion followed by the thread's onto
  segments, a runaway on a fiber, and both halves of the hatch. The late
  invocation of a continuation captured on a segment, which used to assert
  the escape-only error, now asserts the re-entry. `TUR_SEG_CROSS=aarch64`
  runs all of it as arm64 binaries under qemu. New:
  `tests/run-r7rs-segmented-stack-seam.sh` (ctest
  `tur_r7rs_segmented_stack_seam`): every `#lang r7rs` fixture with the flag
  and an 8 MiB base stack (`SEG_ARM=on`), or under the hatch (`SEG_ARM=off`),
  against its expected output, behind a canary that the hatch bites.
- **Results (2026-10-24, Linux x86-64):** the seam, 135 of 135 r7rs fixtures
  on each arm; `run-r7rs-gc.sh` with the flag on (through a `TUR` that adds
  `--enable=r7rs-segmented-stack`), 269 of 269; `bash tests/run.sh` without
  the flag, 4149 passed and 0 failed; the check, natively and as arm64 under
  qemu. At `-O0` with `TUR_SEG_BASE_MB=8` a CPS-colored recursion overflows
  the 8 MiB base stack, as the base stack's note above says it would (its
  calls are real calls there and pass no check); the default 1 GiB base
  stack is why that never shows.

### What landed (2026-10-23)

- **The check** (`TUR_SEG_CHECK`, emitted by `emit_seg_check`,
  `src/compiler/emit_fns.c`): read the stack pointer with a non-volatile asm,
  and while `sp - lo <= span` (unsigned, one thread-local pair) carry on. The
  one compare also fails for a stack pointer on a stack the pair does not
  describe, which is how a `longjmp` out of a segment is noticed. Below the
  limit the procedure re-enters itself, same arguments, through
  `tur_seg_tramp` -- as a tail call (`musttail` under clang), so the slow
  path needs no frame.
- **The switch** (`emit_seg_stack_runtime`, `src/compiler/emit_module.c`):
  the trampoline saves the argument registers, asks `tur_seg_enter` for an
  8 MiB segment (a 64 KiB guard at the bottom, one spare kept per thread),
  copies the stack-passed arguments to its top (a bound from `sizeof` of each
  parameter), calls the procedure there and drops the segment on the way
  back. A check that fails at a stack pointer the current record holds with
  room to spare (a stale pair) re-arms instead; a second failure at the same
  point grows, so nothing re-enters forever. A segment the stack pointer has
  left is dropped at the next failed check.
- **The thread's own stack is not shrunk.** It stays the 1 GiB it was, and
  segments start below it, so code no check covers keeps every byte it had:
  inline C, and a CPS body at `-O0`, whose tail calls are real calls there.
  `TUR_SEG_BASE_MB` shrinks it, for tests.
- **The collector** scans each segment from where its frames start, then the
  one before it from where the frames there resume.
- **`call/cc`** asks the prelude's hooks (`r7k_seg_off_base`, and since
  2026-10-24 `r7k_seg_pieces` and `r7k_seg_restore`), which the runtime sets
  when a thread arms: the prelude's continuation helpers are hoisted above the
  preamble, so an `#ifdef` on the runtime's macro there silently compiles to
  nothing. (A capture on a segment was escape-only until 2026-10-24.)
- **Tests:** `tests/check-r7rs-segmented-stack.sh` (ctest
  `tur_r7rs_segmented_stack`) runs a recursion, collections under torture
  and `call/cc` across segments past an 8 MiB stack, and the budget's
  overflow message; `tests/fixtures/r7rs-segmented-stack-deep-recursion`
  keeps the flag's codegen in `run.sh`. The 136 r7rs fixtures and
  `run-r7rs-gc.sh` (267 cases) pass with the flag on. (Grown on 2026-10-24,
  above.)

### H0: what the check costs

Six call-heavy programs (fib, tak, ack, a 800k-deep `sum-to`, list
build/map/length, nqueens), each reading its input from the command line.
Instruction counts (cachegrind `Ir`, deterministic) against the same program
without the flag:

| | gcc 13 `-O0` | gcc 13 `-O2` | clang 18 `-O0` | clang 18 `-O2` |
| --- | --- | --- | --- | --- |
| fib | +1.4% | +1.4% | +2.4% | +6.8% |
| tak | +1.6% | +1.6% | +2.5% | +3.8% |
| ack | +0.7% | +0.6% | +1.1% | -8.0% |
| deep | +1.1% | +1.2% | +2.0% | -1.5% |
| lists | +0.8% | +0.8% | -1.5% | +0.6% |
| nqueens | +0.4% | +0.9% | +0.7% | -2.8% |

The clang column moves both ways because the check changes clang's inlining
and layout, which matters as much as the compare. Wall clock (best of seven,
interleaved) was within the box's noise, roughly +/-10%.

How it got there, which is the part worth keeping:

1. **A check in every r7rs procedure cost +11-13% (gcc) and +36-52% (clang)
   at `-O2`.** Neither compiler shrink-wraps a prologue past a read of the
   stack pointer -- `__builtin_frame_address` and an asm reading `%rsp` alike
   -- so the prelude's arithmetic helpers, whose fixnum case was frameless
   (`r7rs-lt2__` returned after one push), saved every callee-saved register
   on every call. Making the slow path a tail call did not help: it is the
   read, not the call.
2. **So the check goes only where a cycle needs it**
   (`emit_fn_wants_seg_check`). Every procedure of the program is checked.
   A stdlib function is checked when it makes a call it cannot name (dynamic,
   through a value) or calls inline C that calls back; then, on each cycle of
   direct calls left, one member -- the one with fewest callers, a slow
   path's internal rather than the `=` every comparison goes through -- until
   no cycle is left. 651 checks in a one-procedure program became 133.
3. **No check in a CPS function's direct entry.** Its `__cps` body's calls
   are tail calls into heap continuations already (option A, in effect), and
   a check in the entry stopped clang inlining the body there, which shifted
   the stack enough for the conservative collector to keep 93 MB of dead
   lists alive through one stale word (lists, +139% wall clock).
4. **A slow path at the trampoline's stack pointer, not the check's.** clang
   puts the check after the prologue, gcc before it; a trampoline that
   judged "room left" by its own smaller frame re-entered forever under clang
   (the slack and the second-failure rule above).

The plan's "stop if more than a few percent" bar was met by the selective
check, not by the plain one.

### Open questions, answered

- **How deep do the r7rs fixtures go?** The smallest power-of-two
  `TUR_MAIN_STACK_MB` each of the 136 runs identically under: 125 need 1 MiB,
  the nine `(scheme eval)` fixtures 4 MiB, and the deepest two 32 MiB
  (`r7rs-deep-recursion-million`, `r7rs-tail-call-through-procedure-value`).
  Nothing is near 1 GiB.
- **Does the check block `musttail`?** No. It sits at entry, before
  anything, and its own slow path is a tail call with the procedure's
  signature; a self tail call that became a loop never re-runs it. The
  `run.sh` and `run-r7rs-gc.sh` corpora, tail-call fixtures included, pass
  with the flag on.

### H6: why the JIT has no check

Under `tur jit` the C goes through c2mir (`__MIRC__`), and the whole runtime
is behind `!defined(__MIRC__)`, so `TUR_SEG_CHECK` is `((void)0)` and the
program keeps the engine's own entry thread, sized by `TUR_MAIN_STACK_MB`
(1 GiB by default; `src/jit_engine.c`). Checked on 2026-10-24: the re-entry
program gives the same answer under `tur jit` with and without the flag. Two
things are missing in c2mir, both verified in `external/mir/c2mir/c2mir.c`:

- **No thread-locals.** c2mir parses `_Thread_local` but never lowers it --
  the flag is consulted only by the declaration checks, so a thread-local is
  one process-wide variable. The JIT reaches the runtime's thread-locals
  through host accessor calls instead (`src/runtime/tur_tls.c`). The hot pair
  through an accessor is a call on every checked procedure entry, which H0
  measured is exactly what the check cannot afford: the read of the stack
  pointer already cost +11-52% when it disabled shrink-wrapping, before any
  call.
- **No inline asm.** `asm` is accepted only as a declarator's symbol name, not
  as a statement, so neither the stack-pointer read nor the trampoline nor
  `tur_seg_run_at` can be written in the unit. They would have to be host
  functions resolved through the engine's shims (as `tur_sjlj_x64_win.S` is),
  and the trampoline would then re-enter MIR code through the native ABI
  with its stack-passed arguments copied by size -- MIR's lowering of the
  16-byte by-value `tur_tagged_t` would have to match the C compiler's
  exactly, which nothing pins today.

So H6 is a design of its own (a host-side pair the engine keeps in a register,
or a check MIR emits natively), not a port, and nothing needs it: the JIT's
entry thread is as deep as the compiled path's was before this plan. Deferred
past graduation.

### Platforms: the graduated contract

What graduation turns on, by host. Read from the `#if` structure on
2026-10-24; Linux was run, the rest was compiled or reasoned as stated.

| Host | What a compiled `#lang r7rs` program gets |
| --- | --- |
| Linux x86-64 and arm64, gcc or clang | The check, the segments, every thread armed, fibers checked, call/cc across segments. Run on x86-64 (gcc 13, clang 18, split and whole builds, collector on and off) and as arm64 under qemu (gcc 13, clang 18). |
| macOS (x86-64, arm64) | Nothing of it. `TUR_SEG_STACK_RT` needs `__linux__`, so `TUR_SEG_ON` is never defined: `TUR_SEG_CHECK` falls back to `((void)0)`, `TUR_SEG_THREAD_START` to the deep-stack runtime's `((void)0)`, `tur_deep_size` skips its `TUR_SEG_ON` block (the deep stack is the full 1 GiB or `TUR_MAIN_STACK_MB`), and the `pthread_create` redirect and the fiber swap are inside `#if defined(TUR_SEG_ON)`. The prelude's hooks stay NULL, so call/cc takes exactly its old paths. Not compiled here (no SDK on this box); the C it gets is the Linux C with `__linux__` undefined. |
| Windows (MinGW gcc/clang) | Nothing of it, by the same `__linux__` test. Checked: the C `tur build` compiles for three of the test programs, flag on, compiles for x86_64-w64-mingw32 (gcc 13, `-c`) with no `tur_seg_` symbol in the object, as the flag-off C does. |
| Linux with `__linux__` undefined (a stand-in for "no trampoline here") | Built and run on 2026-10-24: no `tur_seg_` symbol in the binary; a 1e6-deep recursion and eleven re-entries run on the 1 GiB stack, `TUR_SEG_BASE_MB` is ignored, and a thread 1e6 deep dies as without the flag. |
| 32-bit, other architectures, `tur jit`, emscripten, a `--shared` build | Nothing of it (`x86_64`/`aarch64` only; `__MIRC__`; no deep-stack thread; `emit_seg_stack_runtime` writes only the no-op check when `shared`). |
| Any host, built under ASan, HWASan, MSan or TSan | Nothing of it (`TUR_SEG_SANITIZED`, added at graduation). Found by `tests/run-r7rs-sanitize.sh` once the check was on by default: a call/cc re-entry runs `tur_seg_restore_on` on a stack ASan was never told about (`tur_seg_run_at`), and the prelude's `longjmp` back (`r7k_seg_jump`) then aborts inside ASan's `sigaltstack` interceptor (`r7rs-callcc-reentry-many`, `r7rs-gc-basic`). UBSan alone does not track stacks and keeps the check. |

That is the contract: **where the trampoline exists (and no sanitizer tracks
the stack), recursion is bounded by memory on every thread; everywhere else the program is exactly what it was
before this plan, with the deep-stack thread's 1 GiB stack and its overflow
message.** The cost of the check's call sites everywhere else is nothing (the
macro is empty). Segments on macOS (the trampoline's Mach-O symbol spelling,
`_tur_seg_enter`; `pthread_get_stackaddr_np` bounds for the base record) and
on Windows (the ucontext shim or fibers, the TEB's stack bounds, and the
vectored handler for the guard) are work after graduation, each behind its own
proof on that host's CI; neither changes the contract above until it lands.

### H7: sanitizers, what keeping segments would take

Written 2026-10-25, after graduation compiled the segmented stack out of every
build under ASan, HWASan, MSan or TSan (`TUR_SEG_SANITIZED`, `emit_module.c`).
This is what it would take to turn it back on, starting with ASan.

**Already in place.** The ASan side of copying stacks was done with H3,
in the prelude (`stdlib/r7rs/prelude.tur`, under `R7K_ASAN`):

- The copies in and out of a continuation image are `no_sanitize_address`
  (`R7K_NOASAN`, and `TUR_SEG_NOASAN` on `tur_seg_copy`), so reading a
  frame's redzones is not a report.
- After a re-entry copies frames back, `r7k_jump` and `r7k_seg_jump` call
  `__asan_unpoison_memory_region` over every restored piece, so the shadow
  left by whatever ran there before does not describe the restored frames.
- `__asan_default_options` turns `detect_stack_use_after_return` off,
  because ASan's "fake stack" holds address-taken locals where a copy of the
  real stack cannot see them. A user's `ASAN_OPTIONS` still overrides it.

**What is missing: ASan's idea of the current stack.** ASan keeps one stack
range per thread, read at thread start. Every `longjmp` (and every other
no-return path: a `raise` unwinding, `abort`) goes through
`__asan_handle_no_return`, which unpoisons from the current `sp` to the top
of THAT range and, on the way, reads the thread's signal stack through its
own `sigaltstack` interceptor. Execution on any other stack makes those
bounds wrong. Three switches put it there, and none is announced:

1. **The re-entry's temporary stack** -- the one that failed. On an armed
   thread every `call/cc` re-entry goes through `tur_seg_restore`, which
   moves onto a stack just below the image (`tur_seg_run_at`, asm), runs
   `tur_seg_restore_on` there, copies the pieces back and calls the prelude's
   `r7k_seg_jump`, whose `longjmp` aborts inside ASan's `sigaltstack`
   interceptor. This happens with no segment in the chain at all, which is
   why `r7rs-gc-basic` failed without recursing deep: it re-enters a
   continuation on an armed thread, and that is enough.
2. **Entering and leaving a segment** (`tur_seg_tramp` / `tur_seg_enter`).
   Never seen under ASan, because no sanitized run reaches a segment: the
   base stack is 1 GiB and `run-r7rs-sanitize.sh` does not shrink it.
   Expected to fail the same way at the first no-return call made on a
   segment, and an ASan report about a segment address would not know it is
   a stack.
3. **The fiber swap** (`tur_seg_fiber_swap`) moves the check's bounds onto
   a fiber's stack, and the fiber runtime's own `swapcontext` is not
   announced to ASan either. That half predates this plan.

**The fix's shape.** The sanitizer interface pairs
`__sanitizer_start_switch_fiber(&fake, bottom, size)`, called on the old
stack just before the switch, with `__sanitizer_finish_switch_fiber(fake,
&old_bottom, &old_size)`, called first thing on the new one. `fake` can be
NULL while use-after-return is off. If someone turns it on through
`ASAN_OPTIONS`, the runtime should refuse it rather than run with a fake stack
nobody saves. The pairs go at each switch above:

- Re-entry (1): start before `tur_seg_run_at` with the temporary stack's
  bounds; finish at the top of `tur_seg_restore_on`; start again in
  `r7k_seg_jump` before the `longjmp`, with the bounds of the stack it lands
  on (the chain's top piece, or the thread's own stack); finish where the
  capture site's `setjmp` returns non-zero. That last call is the awkward
  one: the landing site is the prelude's generic capture code, not the
  segment runtime, so it has to finish a switch only when one was started.
  A thread-local "switch pending" word handles that.
- Segments (2): the trampolines are asm, so the calls go in C on either
  side. Start in `tur_seg_enter` with the new segment's range, finish in
  the first C the segment runs, and the mirror pair on the way back. A
  procedure that leaves a segment by `longjmp` (a `raise` caught below it, a
  continuation escape) needs the same start-before-jump, finish-on-landing
  as (1).
- Fibers (3): a pair around the fiber runtime's switch, which helps the
  fiber scheduler under ASan with or without segments.

**The other sanitizers.** TSan intercepts `setjmp`/`longjmp` and matches them
by `sp`, so a jump onto another stack confuses its shadow call stack. Its
fiber API (`__tsan_create_fiber` / `__tsan_switch_to_fiber`) is what the
fiber runtime already uses, and it would need one TSan fiber per stack the
chain moves between: separate work, after ASan. HWASan has
`__hwasan_handle_longjmp` and no fiber API. MSan was excluded **by caution,
not by a failure**: nothing was run under it. Narrow `TUR_SEG_SANITIZED` to
ASan and TSan once someone runs MSan with segments on.

**How to test it.** Run `tests/run-r7rs-sanitize.sh` with
`TUR_SEG_BASE_MB=8` in the program's environment, so the deep fixtures
actually cross onto segments under ASan. Build
`tests/check-r7rs-segmented-stack.sh`'s seven programs with
`-fsanitize=address` (re-entry eleven times, threads, fibers, collector
torture). Both runs must pass with ASan removed from `TUR_SEG_SANITIZED`.
Then the Platforms row above shrinks, and the r7rs guide's sentence about
sanitized builds goes.

## Graduation (done 2026-10-25)

The gate drop landed after the 0.71.0 bump, as the owner ruled on
2026-10-24 (the hold condition -- "a real program needs more than 1 GiB of
stack, or a `call/cc`-heavy program's capture cost is dominated by stack
depth" -- lifted; the 8 MiB worker-thread SIGSEGV H4 found is the concrete
case it fixes today). What it did:

- **The registry.** The EXPERIMENTS[] row went to GRADUATED[], and
  `g_opt_r7rs_segmented_stack` was retired with it: `emit_seg_stack_wanted`
  reads only the hatch now, so the check is on for every `#lang r7rs`
  procedure. `--enable=r7rs-segmented-stack` is the TUR-W0063 no-op, eligible
  to age out at 0.72.0.
- **The hatch,** `TUR_R7RS_SEGMENTED_STACK=0`, both halves unchanged: set
  when compiling, no check and no segment runtime (the C every program got
  before); set when running, no thread arms.
- **The seam inverted.** `tests/run-r7rs-segmented-stack-seam.sh` defaults to
  `SEG_ARM=off`, per the flags guide's "Graduating flips a default"; its
  canary now checks that the default build carries the check, the hatch build
  carries neither check nor runtime, and the no-op `--enable` changes nothing.
  `check-r7rs-segmented-stack.sh` builds its "no flag" programs under the
  hatch. The fixture `r7rs-segmented-stack-deep-recursion` lost its `flags`.
- **`check-r7rs-main-stack-size.sh`.** Its "unreservable" case assumed
  `TUR_MAIN_STACK_MB` sizes a stack reserved up front. With segments it is a
  budget and the thread's own stack is capped at 1 GiB, so a huge value runs
  silently; the case now runs on the hatch build where segments are compiled
  in, and the default build elsewhere.
- **Sanitized builds are off** (Platforms, above) -- the one thing graduation
  found. `run-r7rs-sanitize.sh` had never run with the flag on.

The plain-check decision (H0: the check goes only where a cycle needs it)
stands; revisit it if typed code ever wants the check.

### Left, in order

1. **`nightly-arm64.yml`** running the ctest on real arm64 hardware (it ran
   under qemu only), and the seam's both arms there.
2. **macOS segments**, then **Windows segments** (Platforms, above), each
   leaving the contract unchanged until it lands.
3. **H7, sanitizer support**, ASan first: announce every stack switch so an ASan
   build keeps segments instead of compiling them out. What is in place,
   what is missing and how to test it are in
   [H7](#h7-sanitizers-what-keeping-segments-would-take). Until then
   `TUR_SEG_SANITIZED` holds the contract.
4. **H6**, as a design of its own (above), if a JIT user ever needs more than
   the engine's entry stack.
5. **A fiber's stack that grows**: segments for a fiber need the collector to
   find a suspended fiber's segments (a registry it scans, or segments on the
   collected heap), and a migrating fiber's chain to travel with it. Until
   then a fiber stops at its own limit with the message.
6. **A thread leaving by `pthread_exit`** keeps its segments mapped (its
   chain is unmapped only when the start routine returns). A cancelled
   thread's segments are a bounded leak; a key destructor could unmap them.

## Out of scope

- Typed Turmeric code (`#lang turmeric`). Nobody has asked for unbounded
  recursion there, and it would put a check on every call in every program,
  not only Scheme ones.
- Replacing the collector. If B ever becomes interesting, it is a collector
  plan first.

## Open questions

Both of the first draft's are answered under
[Open questions, answered](#open-questions-answered).
