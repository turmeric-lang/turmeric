# `tur jit`: a stack overflow kills `tur` with no message

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
