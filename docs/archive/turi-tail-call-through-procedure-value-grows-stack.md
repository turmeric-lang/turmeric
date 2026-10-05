# turi: a tail call through a procedure value grows the C stack

**Severity:** high under `tur --interpret` -- continuation-passing programs
(SICP 4.3's amb evaluator) overflowed the interpreter's stack.
**Status:** RESOLVED (r7rs-srfi-18-216-sicp-plan, T4 corpus).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (build n k)
  (if (= n 0) (k 0) (build (- n 1) (lambda (v) (k (+ v 1))))))
(write (build 200000 (lambda (v) v)))
```

Compiled: `200000`.  `tur --interpret`: AddressSanitizer stack-overflow in
`eval_lookup` / `eval_expr_impl` -> `turi_call`.

## Root cause

The interpreter's work-stack driver (`eval_drive_ex`, src/turi/eval.c) gives
`EX_CALL` proper tail calls through `DK_CALL_ARG` (tail calls reuse the
enclosing `DK_CALL_RET`).  `EX_DYN_CALL` -- a call whose callee is a value,
which in `#lang r7rs` is every `(k v)` -- had no driver case, so it fell
through to `eval_expr_impl`, which evaluates the arguments and then calls
`turi_call` recursively: one C frame per call, tail or not.

## Fix

A driver case for `EX_DYN_CALL` that evaluates the callee, then pushes the
same `DK_CALL_ARG` frame `EX_CALL` does.  The handler reads its arguments
through `drive_call_n_args` / `drive_call_arg` (either node kind), packs a
variadic callee's surplus into its rest chain (what `eval_expr_impl`'s arm
did), and skips the static-dispatch bookkeeping that only a static call
carries (poly-call dict actuals, ABI pins).  The capture/clone paths size the
accumulator through the same helper.

Fixture: `tests/fixtures/r7rs-tail-call-through-procedure-value`.
