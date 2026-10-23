# Plan: Scheme recursion depth bounded by memory, not by the C stack

> **Status:** IN PROGRESS (prototype) behind `--enable=r7rs-segmented-stack`
> since 2026-10-23. Every stage has landed for Linux x86-64 and arm64 (arm64
> run under qemu-aarch64, gcc and clang), compiled and under `tur jit`. macOS
> and Windows are open. See [Progress](#progress-2026-10-23).
> **Type:** Compiler / r7rs runtime
> **Predecessors:** `docs/archive/r7rs-deep-recursion-segfaults-silently.md`
> (PR #1082: `main` runs on a 1 GiB thread, and an overflow prints a message).
> **Was on hold until:** a real program needs more than 1 GiB of stack, or a
> `call/cc`-heavy program's capture cost is dominated by stack depth. It was
> picked up on request; graduation still wants one of those.

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

1 GiB is deeper than any program has needed so far (H0 measured the r7rs
fixtures: none needs more than 32 MiB). This was the choice until C was
prototyped; it is still the default, since C is behind a flag.

## Stages for option C

Each stage lands separately, behind `--enable=r7rs-segmented-stack` until it
graduates (CLAUDE.md, "Experimental Compiler Features"). The stages as first
planned; [Progress](#progress-2026-10-23) says where each stands.

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

## Progress (2026-10-23)

| Stage | State |
| --- | --- |
| H0, measure | Done. Numbers below; the check costs "a few percent" or less once it is placed with care. |
| H1, the switch | Done, Linux x86-64 and arm64, the deep-stack thread. |
| H2, the collector | Done. `r7gc.c` walks the segment chain (`tur_gc_scan_segments`). |
| H3, `call/cc` | Done. A capture on a segment copies the chain down to the thread's own stack, and a re-entry maps each segment back at its address. |
| H4, threads and platforms | Done on Linux: every thread the program starts has a chain of its own, and a fiber runs unchecked on its own stack. macOS and Windows compile no check (below). |
| H5, the cap | Done. `TUR_MAIN_STACK_MB` is the total; unset, half of physical memory. |
| H6, the JIT | Done for the engine's entry thread, which runs `main`. Threads a JIT'd program starts run unchecked. |

### What landed

- **The check** (`TUR_SEG_CHECK`, emitted by `emit_seg_check`,
  `src/compiler/emit_fns.c`): read the stack pointer with a non-volatile asm,
  and while `sp - lo <= span` (unsigned, one thread-local pair) carry on. The
  one compare also fails for a stack pointer on a stack the pair does not
  describe, which is how a `longjmp` out of a segment is noticed. Below the
  limit the procedure re-enters itself, same arguments, through
  `tur_seg_tramp` -- as a tail call (`musttail` under clang), so the slow
  path needs no frame.
- **The switch** (`src/runtime/r7seg.c`, which `emit_seg_stack_runtime`
  embeds):
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
- **`call/cc`** reaches the runtime through weak functions
  (`tur_seg_r7k_off_base`, `tur_seg_r7k_capture`, `tur_seg_r7k_restore`):
  the prelude's continuation helpers are hoisted above the preamble, so an
  `#ifdef` on the runtime's macro there silently compiles to nothing, and so
  did the first version's hook pointer, which the runtime set too late.
- **H3, the chain in the image.** A capture on a segment copies the live part
  of each segment, with its record, down to the thread's own stack; the
  usual copy of the thread's own stack starts where the oldest segment's
  frames resume. A re-entry needs each segment at its old address, since the
  frames point into it: a segment address is released only into a per-thread
  pool of reserved slots (`PROT_NONE`, memory returned), so it is reused only
  as the same whole slot, and the re-entry takes it from the chain, the
  spare or the pool, or maps it again with `MAP_FIXED_NOREPLACE` (and stops
  with a message if something else took it). The rebuild runs on the
  thread's own stack below the image, then copies the image and jumps.
- **H4.** `tur_gc_thread_main` arms each started thread (its own chain, a
  signal stack so its overflow prints the message, everything unmapped when
  it ends). A fiber's stack is a heap block no record describes, so the
  resume disarms the pair for the fiber and restores it after. glibc's
  fortified `longjmp` (`__longjmp_chk`) refuses a jump to a lower stack
  pointer, which a jump from a segment into the thread's own stack is when
  the segment sits higher (qemu-aarch64 puts it there): `TUR_LONGJMP` and
  `R7K_LONGJMP` call the unchecked symbol.
- **H6, the JIT.** c2mir has no thread-locals, no inline asm, no `typeof`
  and no sibling calls. So `tur` compiles `r7seg.c` itself (`TUR_SEG_HOST`):
  the state and the trampoline live in the engine, which arms its entry
  thread (`jit_run_entry`; the entry stack is the size of the cc path's,
  `TUR_SEG_BASE_MB` included), and the program's check reads the pair
  through one call (`tur_seg_jit_hot`) and the stack pointer from a local's
  address. Its slow path calls the trampoline through the procedure's own
  type, which the emitter writes ahead of it as `tur_seg_fnty_<name>`. The
  r7k call/cc hooks are the host's too. Because MIR makes no sibling calls,
  two cycles gcc and clang close flat need a check there: a CPS procedure's
  `return g__cps(...)`, and running a continuation chain (`dk_run_impl` ->
  a frame's function -> `dk_run`, which SICP's `map` does once per element
  on the way back up); the second is `TUR_SEG_CHECK_JIT`, empty natively.
- **A CPS procedure's body is checked natively too.** A call it makes for a
  value goes through the callee's direct entry, which has no check, so
  SICP's `accumulate` -- `(op (car l) (accumulate op init (cdr l)))` --
  recursed accumulate__cps -> accumulate -> accumulate__cps with none on
  the way, and overflowed the thread's own stack with the flag on. (The H0
  note below was about the direct entry, and still holds.)
- **Tests:** `tests/check-r7rs-segmented-stack.sh` (ctest
  `tur_r7rs_segmented_stack`) runs, past an 8 MiB stack: a recursion,
  collections under torture, `call/cc` across segments and captured on them
  (escaping, and re-entered late over other frames), two threads, a fiber,
  SICP's list functions, and the budget's overflow message -- compiled, and
  again under `tur jit` (all but the threads; a JIT that fell back to cc
  fails the case). `nightly-arm64.yml` runs it on an arm64 runner.
  `tests/fixtures/r7rs-segmented-stack-deep-recursion` keeps the flag's
  codegen in `run.sh`. The r7rs fixtures (`run.sh` and `run-jit.sh`) and
  `run-r7rs-gc.sh` (268 cases) pass with the flag on.

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
3. **No check in a CPS function's direct entry.** A check in the entry
   stopped clang inlining the body there, which shifted the stack enough for
   the conservative collector to keep 93 MB of dead lists alive through one
   stale word (lists, +139% wall clock). The `__cps` body is checked instead
   (H3/H6 work, above): its calls to other CPS procedures are sibling calls,
   but a call it makes for a value goes through a direct entry.
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

### The cost, after H3-H6

Best of five, wall clock, Release `tur`, flag off against on, 4-core x86-64
(the JIT's includes its ~1.1 s compile):

| | cc | `tur jit` |
| --- | --- | --- |
| fib 33 | 0.41 s -> 0.42 s (+4%) | 2.41 s -> 2.47 s (+3%) |
| tak 30 20 10 | 2.87 s -> 3.04 s (+6%) | 12.93 s -> 12.80 s (-1%) |
| SICP lists, 1e6 | 1.77 s -> 1.73 s (-2%) | 3.96 s -> 4.06 s (+2%) |

The JIT's check is a call into the engine per procedure entry, which is
lost in MIR's own call cost.

### A program that needs it

`sum-odd-squares` from SICP 2.2.3, written the textbook way -- the
signal-flow version, every list function non-tail-recursive:

```scheme
(define (enumerate-interval lo hi)
  (if (> lo hi) '() (cons lo (enumerate-interval (+ lo 1) hi))))
(define (my-map f l)
  (if (null? l) '() (cons (f (car l)) (my-map f (cdr l)))))
(define (my-filter keep? l)
  (cond ((null? l) '())
        ((keep? (car l)) (cons (car l) (my-filter keep? (cdr l))))
        (else (my-filter keep? (cdr l)))))
(define (accumulate op initial l)
  (if (null? l) initial (op (car l) (accumulate op initial (cdr l)))))
(define (sum-odd-squares n)
  (accumulate + 0 (my-map (lambda (x) (* x x))
                          (my-filter odd? (enumerate-interval 1 n)))))
```

At n = 3e6 it outgrows the default 1 GiB stack ("stack overflow: recursion
too deep" after 4.6 s). With the flag it prints 4499999999999500000 in 5.7 s
at a 1.76 GiB peak (`tur jit`: 10.7 s, 3.0 GiB); n = 1e7 takes 38.5 s and
5.5 GiB. `accumulate` holds 3e6 frames of its own at the deepest point, each
with a direct entry's landing (a `jmp_buf`), which is why the frames are
large. It is a toy, not the "real program" the graduation condition asks
for, but it is the program every SICP reader writes first.

### Left, in order

- **macOS.** Not doable from the Linux sandbox this was built in (no
  Darwin SDK or runner). Needed: the Mach-O spelling of the trampoline's
  symbols (`_tur_seg_tramp`, `_tur_seg_enter`), `pthread_get_stackaddr_np`
  / `pthread_get_stacksize_np` bounds instead of `pthread_getattr_np`,
  `MAP_ANON`, no `MAP_FIXED_NOREPLACE` (a hinted `mmap` checked for the
  address it got), and arm64's AAPCS64 variant (Apple passes variadic
  arguments on the stack, and stack arguments are packed to their natural
  size, so the `nb` bound stays an upper bound but the copy is the same).
- **Windows.** The cross toolchain is available (mingw-w64, Wine), but the
  work is a port, not a build: a Win64 trampoline (rcx/rdx/r8/r9 and
  xmm0-3, the 32-byte shadow space, stack arguments above it), segments from
  `VirtualAlloc` with a `PAGE_GUARD` page, and the TEB's `StackBase` /
  `StackLimit` switched with the stack -- `__chkstk`, SEH and the CRT's
  `longjmp` (which unwinds through `RtlUnwindEx`) all read them, so a
  segment they do not describe faults or aborts. Wine's TEB and unwinder
  are not a faithful enough stand-in to trust a pass there; it wants a
  Windows runner.
- **Threads under `tur jit`.** A thread a JIT'd program starts runs
  unchecked on its own stack. Arming it needs a hook from the program's
  thread start into the engine (`tur_gc_thread_main` is in the program,
  and the collector is off under the JIT).
- **Graduation** wants a program that needs it (the hold condition above),
  then the plain-check decision revisited if typed code ever wants it.

## Out of scope

- Typed Turmeric code (`#lang turmeric`). Nobody has asked for unbounded
  recursion there, and it would put a check on every call in every program,
  not only Scheme ones.
- Replacing the collector. If B ever becomes interesting, it is a collector
  plan first.

## Open questions

Both of the first draft's are answered under
[Open questions, answered](#open-questions-answered).
