# CPS backend: two abortive resets plus a capture, and a float through an escape `k`

**RESOLVED 2026-10-21** (items 1, 2 and the `call/cc` argument check of item 5 of
continuation-forms-gaps). The rest stays open in
[continuation-forms-gaps](../reported/continuation-forms-gaps.md). Pinned by
`cps-two-resets-with-capture` and `escape-float-through-k` (compiled, and
interpreted where the program is not compiled-only) and
`errors/callcc-literal-receiver`.

## What was wrong

1. **A function with two or more abortive resets and a capture did not compile.**
   `[EVICT] BODY-STRUCT-CORE` named it. The structural check is
   `reset_body_ok` -> `joins_closed_rec_x` (`emit_cps_ir.c`): a reset's
   continuation must be self-contained, and the walk had no case for a `reset`
   or an abortive `shift` in that continuation, so a second reset (an operand or
   the next statement) made it `false` and the function left the CPS subset. The
   direct emitter that got it cannot lower `call/cc`, `escape`, `cloneable-shift`
   or `serial-shift`, hence `EX_CALLCC reached the direct emitter` / TUR-E0710 /
   TUR-E0706. Two resets alone never reached the check, since nothing in such a
   function needed the CPS backend. Fix: a nested `reset` is walked like a nested
   `handle` (its delimited body in a fresh join scope, its continuation in the
   same one), and an abortive `shift`, whose body is straight-line, is closed.
2. **A float passed to `k` of a compiled `call/cc` / `escape` printed as garbage.**
   The landing's `result` is an int64 holding the float's bits and was read with
   a value conversion (`(double)result`); it is a reinterpret now, for `double`
   and `float` (`emit_cps_callcc`). `check-emitted-float-conversions.py` is clean.
3. **`(call/cc 5)`** was accepted by `tur check`, an internal emitter error when
   compiled and `call/cc expects a function` when interpreted. A literal, bool,
   float or string receiver is TUR-E0001 at elaboration (`callcc_node`).

The guide's claim that an unannotated `k` called twice is TUR-E0100/E0101 was
wrong (only `^linear` is checked, and the first `(k v)` leaves the call, so the
second never runs); effects-system-guide now says so.

Left as a nit: `(k 2.25)` for an `escape-cont<float32>` passes the double literal's
bits, so the float32 landing reads 0; a `float32`-typed argument works.
