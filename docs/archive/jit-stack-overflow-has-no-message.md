# `tur jit`: a stack overflow kills `tur` with no message

**RESOLVED 2026-10-07.** The JIT's entry thread now carries the overflow
handler (`jit_run_entry`, `src/jit_engine.c`): an alternate signal stack and
a `SA_ONSTACK` handler for SIGSEGV and SIGBUS, installed around the run and
restored after the join. A fault near the low end of the entry thread's stack
prints `stack overflow: recursion too deep`, then the signal's default action
is restored so the fault repeats and the exit status is the signal's, as
before. Any other fault is handed back to the handler that was there before
(the default, or ASan's on a Debug `tur`) and repeats there, so a wild
pointer in JIT'd code reports exactly as it did. The thread's previous
alternate stack is put back before the thread ends (ASan gives each thread
its own and frees it at thread exit).

The test and the message are one definition now, `src/runtime/stack_overflow.h`
(`TUR_STACK_FAULT_IS_OVERFLOW`, `TUR_STACK_OVERFLOW_MSG`): the JIT uses them
directly and `emit_deep_stack_runtime` pastes them into a compiled r7rs
program as text, so the two cannot drift.

Measured on Linux x86-64, Debug `tur` (ASan): both repros below, under
`TUR_MAIN_STACK_MB=8 tur jit`, print the line and exit 139; before, ASan
printed its own `stack-overflow` report and `tur` exited 1 (a Release `tur`
printed nothing). A read of address 8 in inline C still reaches ASan's SEGV
report and does not print the line. Pinned by
`tests/check-jit-stack-overflow-message.sh` (ctest
`tur_jit_stack_overflow_message`, SKIPs without the JIT), which fails on the
old engine. `TUR_MAIN_STACK_MB` is the size knob from
[r7rs-deep-stack-size-not-configurable](r7rs-deep-stack-size-not-configurable.md);
the report's suggested `TUR_JIT_STACK_MB` still works too.

**Not verified here:** macOS (SIGBUS is handled and the bounds come from
`pthread_get_stackaddr_np`, as in the cc path, but no macOS run was made);
CI's macOS JIT leg runs the check. **Not done:** Windows. The JIT's Windows
entry thread has no vectored handler yet; the cc path's
`EXCEPTION_STACK_OVERFLOW` handler is the model if it is wanted.

**Severity:** low-medium (diagnostic quality). Under `tur jit`, a recursion
that runs out of stack ends the whole `tur` process with a bare signal and
nothing on stderr, in every dialect. PR #1082 gives a compiled `#lang r7rs`
program the message `stack overflow: recursion too deep`, but says the JIT
still has none. This report is that gap, and it is wider than r7rs. Filed
2026-10-05 reviewing #1082.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (down n) (+ 1 (down (+ n 1))))
(write (down 0))
(newline)
```

```turmeric
(defn down [n : int] : int (+ 1 (down (+ n 1))))
(defn main [] : int (println (down 0)) 0)
```

Measured 2026-10-05 on `origin/main` 59d1dfe5c, macOS arm64, Debug `tur`
built with `-DTUR_JIT=ON -DTUR_DEBUG_SANITIZE=OFF`:

| Run | Result |
|---|---|
| `tur jit runaway.scm` | exit 138 (SIGBUS), no message |
| `tur jit runaway.tur` | exit 138 (SIGBUS), no message |
| `tur build runaway.scm`, then run it | exit 139 (SIGSEGV), no message (the gap #1082 closes for the cc path) |

stderr held only c2mir's compile warnings in each case. On macOS the guard
page fault arrives as SIGBUS, so a fix has to catch SIGBUS as well as SIGSEGV,
as #1082's handler does. Linux was not measured.

## Root cause

`tur_jit_execute` (`src/jit_engine.c`) runs the program's `main` on a
pthread whose stack is `TUR_JIT_STACK_MB` (64 MiB on `main`, 1 GiB after
#1082). Nothing installs an alternate signal stack or a fault handler on that
thread. #1082's handler is emitted into the program's C behind
`#if !defined(__MIRC__)`, so the JIT never compiles it. And under the JIT the
process that dies is `tur` itself, so a handler in the program's text would be
the wrong place anyway.

## Fix directions

- In `tur_jit_execute`, on the thread that runs `main`: `sigaltstack` plus a
  `SA_ONSTACK` handler for SIGSEGV and SIGBUS that prints the same
  `stack overflow: recursion too deep` when the faulting address is near the
  low end of that thread's stack (its bounds are known, since the engine
  created it), then restores the default action and re-raises, so the exit
  status is unchanged. This covers every dialect at once.
- Factor the fault test (address within a few pages of the stack's low end)
  so that the cc path and the JIT share one definition and one message.
- A fault that is not a stack overflow (a real wild pointer in JIT-compiled
  code) should keep its current behavior; a later improvement could say
  "crashed in JIT-compiled code" there too.
- Pin it with a `tests/run-jit.sh` case that sets `TUR_JIT_STACK_MB` small
  (for example 8) so that reaching the overflow is fast and commits little
  memory, and asserts the message plus the signal exit status.
