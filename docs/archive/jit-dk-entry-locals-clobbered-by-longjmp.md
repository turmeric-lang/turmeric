# `tur jit`: a DK entry's driver landing restored a stale pending-perform list

**RESOLVED 2026-10-22** (archived the day it was found). Found while landing the
CPS `catch-unwind` lowering
([cps-catch-unwind-cannot-cross-an-effect-capture](../reported/cps-catch-unwind-cannot-cross-an-effect-capture.md)):
`defer-cps-caught-panic` started failing under `tests/run-jit.sh` once its
catch-unwind thunks became part of their callers' CPS bodies. The defect was
already on `main` (`a0a718f3`); the new lowering only routed that fixture
through it.

**Severity: high** under the JIT (a heap-use-after-free, so silent memory
corruption in a Release `tur`); none under `tur run`.

## Repro

`tests/fixtures/dk-entry-landing-restores-pending-perform`: a function whose
`handle` has a tail-resume case (`(resume k 1)`), called from a plain `main`,
then a function whose `handle` has an abortive case. Under `tur jit` (Debug,
ASan-built runtime) the second one stops with

```
ERROR: AddressSanitizer: heap-use-after-free ...
    #0 __dk_abort_pending
    #1 dk_perform
```

`tur run` and `tur --interpret` print the right lines.

## Root cause

A direct-to-CPS entry wrapper (`emit_cps_ir.c`, the D2b `main` wrapper and the
generic one) saves the enclosing driver and `g_dk_pend_perform` in locals,
installs its own driver with `TUR_SETJMP`, and on the landing a tail resume
longjmps to, restores `g_dk_pend_perform` from the saved local before running
the trampoline. Under the JIT the wrapper is compiled by c2mir. Its setjmp did
not preserve that local, and the landing restored `g_dk_pend_perform` to the
address of a DK node. The next abortive case (`dk_perform` ->
`__dk_abort_pending`) walked that node as a pending-perform record and read
its freed handler. Marking the locals `volatile` did not help.

## Fix

Everything the landing reads back (the enclosing driver, the pending-perform
list, the root prompt, the reap mark) now lives in one struct with the jump
buffer, whose address is taken, so no backend keeps it in a register
(`emit_dk_entry_landing_state`). Snapshots regenerated.
