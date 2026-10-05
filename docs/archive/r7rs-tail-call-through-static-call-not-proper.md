# r7rs (compiled): a tail call reaching a procedure value through a static call grows the C stack

**Severity:** high -- SICP 4.3's amb evaluator overflowed an 8 MiB stack
(exit 139 on macOS, where the Linux stack re-exec does not apply).
**Status:** RESOLVED (r7rs-srfi-18-216-sicp-plan, T4 corpus).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (step n k)
  (if (= n 0) (k 0) (step (- n 1) (lambda (v) (go k v)))))
(define (go k v) (k (+ v 1)))
(write (step 1000000 (lambda (v) v)))
```

`TUR_STACK_REEXEC=0` with an 8 MiB stack: "stack overflow (recursion too
deep)", exit 139.  Also `((lambda (m) (loop m k)) (- n 1))` in tail position.

## Root cause

Dynamic tail calls are proper through the trampoline (proper-tail-calls T6):
a function whose tail reaches a dynamic call is a *bouncer*; a driver arms it
on entry (`tur_tb_armed_for`), and an armed bouncer returns its dynamic tail
call to the driver instead of making it.  The continuation lambda above
tail-calls `go` STATICALLY, so it was no bouncer, nothing armed `go`, and
`go`'s own `(k ...)` made a fresh driver -- one C frame pair per step.

An immediately-applied lambda was lowered as a call of a closure value that
was never a C tail call at all.

## Fix

- `emit_expr.c` `tb_tail_reaches_dyn_call`: a static tail call to a bouncer
  makes the caller a bouncer too (bounded at four hops).
- `emit_fns.c` `emit_tail_hand_on_arming`: an armed caller arms the callee
  right before the C tail call; the callee consumes the arming on entry and
  any bounce it returns is the caller's own value, so it reaches the same
  driver.
- `scheme_lower.c`: `((lambda (x ...) body...) a ...)` with a fixed formals
  list lowers as `(let ((x a) ...) body...)`, as R7RS defines `let`.

Fixture: `tests/fixtures/r7rs-tail-call-hand-on-through-static-call`, also run
in an 8 MiB stack by `tests/check-r7rs-deep-recursion.sh`.
