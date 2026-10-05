# `#lang r7rs`: deep non-tail recursion kills a compiled program with no message

**Severity:** medium. A compiled Scheme program that recurses deeply in
non-tail position (1,000,000 frames) dies of SIGSEGV with no output at
all: no "stack overflow", no Scheme error. A student sees a program that
printed nothing and exited 139. SICP 1.2.1 teaches exactly this shape
(the linear recursive process) and invites trying it on large inputs. `tur
--interpret` answers correctly. Found 2026-10-03 probing SICP code
(docs/archive/r7rs-srfi-18-216-sicp-plan.md).

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

## Resolution (2026-10-04)

Both directions, in stdlib/r7rs/stack.tur (a C block the prelude loads, so
every compiled `#lang r7rs` program carries it):

- **Recurse further (Linux).** A startup constructor raises RLIMIT_STACK's
  soft value to 1 GiB (or the hard limit) and re-executes the program once
  through /proc/self/exe, so the kernel lays out the address space for the
  new limit; threads keep an 8 MiB default (`pthread_setattr_default_np`).
  The report's repro now prints `500000500000`. `TUR_STACK_REEXEC=0` turns it
  off. macOS fixes a main thread's stack at link time, so it is unchanged
  there.
- **Say what happened (Linux and macOS).** A SIGSEGV handler on an
  alternate stack recognizes a fault within a few pages of the faulting
  thread's stack pointer and writes `error: stack overflow (recursion too
  deep)` before letting the signal end the program (still exit 139). A
  handler the program installed itself is left alone.

Measured: the full fixture suite and `tests/run-r7rs-gc.sh` (threads, fork,
the `ulimit -v` reclamation cases) pass; each program start pays one extra
exec. A runaway recursion now touches up to 1 GiB before it stops, where it
used to stop at 8 MiB. Pinned by `tests/check-r7rs-deep-recursion.sh`
(ctest `tur_r7rs_deep_recursion`).
