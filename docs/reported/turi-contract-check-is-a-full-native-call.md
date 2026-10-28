# The interpreter pays a full native call for every contract check

**Severity:** low (performance, `tur --interpret` and the REPL only).

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
