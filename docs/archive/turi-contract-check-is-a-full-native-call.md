# The interpreter pays a full native call for every contract check

**Severity:** low (performance, `tur --interpret` and the REPL only).

**RESOLVED 2026-11-05.** The driver recognises the two natives (`contract_check_native`,
`src/turi/eval.c`). At `EX_CALL` setup, a condition that cannot perform
(`ws_has_perform`) is evaluated on the spot -- no argument buffer, no
work-stack frame -- and a holding check returns nil without evaluating the
message, file and line or making the call. A condition that may perform is
driven on the work stack as before, and `DK_CALL_ARG` skips the remaining
arguments and the call once it holds. A failing check takes the native, so the
panic text and site are unchanged. Pinned by
`tests/fixtures/turi-contract-check-fast-path` (both engines).

Re-measured with the Debug `tur` on the same shape (3e5 iterations): the loop
with `(tur-contract-check-at (> r 0) ...)` went from +0.73 s over the bare loop
to +0.30 s, the same as `(if (> r 0) 0 (panic "x"))` (+0.30 s) and `(> r 0)`
alone (+0.27 s) -- the report's target.

## Measurement

`tur --interpret`, a 1e6-iteration loop around one call to a trivial function,
three runs each, adding one form per iteration (Release `tur`, 2026-10-28):

| form added per iteration | extra per iteration |
|---|---|
| `(> r 0)` | +0.04 us |
| `(if (> r 0) 0 (panic "x"))` | +0.10 us |
| `(chk (> r 0))`, a trivial user `defn` | +0.23 us |
| `(tur-contract-check (> r 0) "msg")` | +0.21 us |
| `(tur-contract-check-at (> r 0) "msg" "f.tur" 3)` | +0.29 us |

Every refinement, `:pre`, `:post`, loop-invariant and refined function-type
check is a `tur-contract-check-at` call. So under the interpreter a check costs
about three times its own condition. In a tight loop that roughly doubles the
time: refined-function-types-plan F4 measured a loop whose call-site check took
it from about 500 ms to about 1000 ms
([benchmarks/refined-fn-result-check-results.md](../../benchmarks/refined-fn-result-check-results.md)).
Compiled code does not pay this: the check is an inlined compare.

## Root cause

`tur-contract-check` and `tur-contract-check-at` are registered natives
(`src/turi/interpreter_natives.c:7067`, `:7090`, registered at `:7635`), so
each check goes through the generic native-call path. That path evaluates all
two or four arguments, including the message and file-name strings, and makes
the call, even though the condition is true on almost every execution.

## Fix directions

- Recognize a call to the contract-check natives in the evaluator, as the
  emitter in effect does: evaluate the condition, and only when it is false
  evaluate the remaining arguments and raise the panic. That should bring a
  check down to roughly the `if` + `panic` row.
- Keep the panic text and location identical, so `run-turi.sh` and `run.sh`
  stay in agreement (`tur-contract-check-at`'s file and line are part of the
  message the fixtures pin).
