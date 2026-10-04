# `#lang r7rs`: deep non-tail recursion kills a compiled program with no message

**RESOLVED 2026-10-04, both fix directions.** A `#lang r7rs` program's emitted `main` now starts with `TUR_DEEP_STACK_ENTER(argc, argv)`, which re-enters `main` on a thread with a 1 GiB stack. That is address space, and only what the recursion touches is committed. The process exits from that thread, as returning from `main` would. The runtime half is `emit_deep_stack_runtime` in `src/compiler/emit_module.c`, emitted after the collector's paste, so `pthread_create` is the collector's registering wrapper. The collector and `call/cc` already ask pthreads for the calling thread's stack bounds, so they follow the move. The original thread leaves the collector's registry (`tur_gc_leave_thread`, `src/runtime/r7gc.c`) before it waits in an unwrapped join, so it is neither scanned nor counted as a live thread. That count is what `r7rs-threads-lifecycle` polls, and it hung the first attempt. On the big thread, a SIGSEGV/SIGBUS handler on an alternate stack prints `stack overflow: recursion too deep` for a fault just past the stack's low end, then lets the signal kill the process as before (exit 139).

Linux, gcc 13, Debug `tur`: `(sum-to 1000000)` prints `500000500000` in 0.09 s; 3e6, 1e7 and 2e7 all finish; 4e7 prints the overflow message. Windows has its own branch: `CreateThread` with a 1 GiB `STACK_SIZE_PARAM_IS_A_RESERVATION` stack, with no collector to register with since `TUR_GC_ON` is 0 there. A vectored handler for `EXCEPTION_STACK_OVERFLOW` prints the same message, and `SetThreadStackGuarantee` leaves it room to run. #1082's first CI run failed the fixture on Windows 3/3 because that branch was missing. It was checked with mingw-w64 (`-Wall`, clean) and Wine 9: a C recursion 1e6 deep through the emitted helper prints its answer; 1e7 prints the overflow message; with `TUR_NO_DEEP_STACK=1`, 1e6 fails. The helper is not used under `tur jit` (`__MIRC__`), whose engine already runs `main` on a sized thread. Its default (`TUR_JIT_STACK_MB`, `src/jit_engine.c`) goes from 64 MiB to the same 1 GiB on 64-bit: the fixture overflowed 64 MiB under the JIT (exit 1), because MIR frames are larger than gcc's. Under the JIT an overflow has no message yet. `TUR_NO_DEEP_STACK=1` turns it off. If the thread cannot be made, `main` runs where it is, as before. Pinned by `tests/fixtures/r7rs-deep-recursion-million` (a million-deep sum). It fails with exit 139 under `TUR_NO_DEEP_STACK=1`. The fixture allocates nothing on purpose: `tests/run-r7rs-gc.sh` collects every 31 allocations, and each collection scans the whole live stack, so a million allocations a million frames deep is quadratic (a `cons` recursion timed out there at 300 s). Outside torture, a deep *allocating* recursion pays the same per-collection stack scan, just far less often. **Not verified here:** macOS (it uses `pthread_get_stackaddr_np`/`pthread_get_stacksize_np` for the bounds; CI's macOS legs run the fixture), and the overflow message as a fixture (reaching it commits the full 1 GiB). The `sicp-guide` rough-edges entry is gone; chapter 1 and "When something goes wrong" now give the limit.

**Severity:** medium. A compiled Scheme program that recurses deeply in
non-tail position (1,000,000 frames) dies of SIGSEGV with no output at
all: no "stack overflow", no Scheme error. A student sees a program that
printed nothing and exited 139. SICP 1.2.1 teaches exactly this shape
(the linear recursive process) and invites trying it on large inputs. `tur
--interpret` answers correctly. Found 2026-10-03 probing SICP code
(docs/upcoming/r7rs-srfi-18-216-sicp-plan.md).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (sum-to n) (if (= n 0) 0 (+ n (sum-to (- n 1)))))
(write (sum-to 1000000))
(newline)
```

| Back end | Result |
|---|---|
| compiled, `n` = 100,000 | `5000050000` |
| compiled, `n` = 1,000,000 | nothing printed, exit 139 (SIGSEGV), macOS arm64 |
| `tur --interpret`, `n` = 1,000,000 | `500000500000` |

(Measured on origin/main b87ca3553, Debug build without sanitizers.)

## Root cause

The C stack runs out: each Scheme frame is a C frame, and the main thread's
default stack is 8 MiB on macOS. There is no `sigaltstack` handler anywhere
in `src/` to turn the guard-page fault into a message.

## Fix directions

Pick one or both:

- **Say what happened.** Install a SIGSEGV handler on an alternate stack in
  r7rs programs that recognizes a fault in the stack guard region and prints
  `stack overflow (recursion too deep)` before exiting. That costs nothing
  per call.
- **Recurse further.** Run the program body on a thread with a much larger
  stack (Racket, Chez and Guile all let this depth succeed; chibi grows its
  stack on the heap). A 1 GiB reserved stack is only address space until it
  is touched.

Whichever lands, the guide's SICP section should say where the limit is.

## Guide upkeep

`docs/guides/sicp-guide.md` documents this defect in its "Rough edges" list
(and the workaround in the chapter it affects). When this is fixed, delete
that entry and any workaround text that only exists because of it.
