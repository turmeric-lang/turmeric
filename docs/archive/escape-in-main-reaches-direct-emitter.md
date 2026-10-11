# `escape` / `call/cc` written directly in `main` is an internal error, not a diagnostic

**Severity: low-medium (internal error on a legal program; trivial workaround).**
Found 2026-10-11 while adding escape shapes to
`tests/fixtures/defer-cps-abortive-handler`.

## Repro

```turmeric
(defn main [] : int
  (println (+ 100 (escape (fn [k] (+ 3 (k 9))))))
  0)
```

```
tur: emit: EX_CALLCC reached the direct emitter (should be handled by the CT-IR backend)
```

The same form inside any other `defn` works (`tests/fixtures/continuation-escape`):
the function is CPS-lowered and `escape` has a native landing there
(`emit_callcc`, src/compiler/emit_cps_ir.c).  Moving the `escape` into a
helper is the workaround.

## Root cause

A `main` that is not the d2b shape is permanently routed to the direct
emitter (`SIG-MAIN` in the `TUR_TRACE_EVICT=1` trace, emit_cps_ir.c
`fn_is_main && !fn_is_d2b_main`), and the direct emitter has no lowering for
`EX_CALLCC` -- its arm (src/compiler/emit_expr.c) prints the message above
and calls `abort()`: the compiler dies on a legal program with no
diagnostic and no source location.

## Fix directions

- Lift an `escape` / `call/cc` that appears in a direct-only function into a
  synthesized CPS helper (the body of the escape site as a zero-arg
  colored thunk, called through the ordinary direct->cps entry wrapper),
  the way a `main` with effects is already entered through d2b.
- At minimum, make the arm a real `TUR-E` diagnostic that names the
  workaround and stops the build, instead of a message on stderr and an
  empty expression.

## Resolved 2026-10-11

Direction 1, at the root: `fn_is_d2b_main` (src/compiler/emit_cps_ir.c)
now makes a zero-arg `main` a CPS candidate when its body directly holds a
`call/cc` / `escape`, as it already did for a reset / shift or a `handle`.
The test is `cps_expr_contains_callcc` (src/passes/cps.c), the coloring
walk `cps_directly_uses_control` parameterized to seed on `EX_CALLCC`
alone, so it walks exactly the operand positions the coloring pass walks
(call arguments, builtins, `match` arms, the Saffron `any` seams, ...)
rather than a second hand-written subset.  The candidate goes through the
same taint fixpoint and falls back the same way as a handle-installing
main.  Fixture `tests/fixtures/escape-in-main`: the escape taken, the
receiver ignoring `k`, the `call/cc` spelling in a `let`, and the escape
continuation reaching a handler case in a callee (the perform it jumps
over fires its pending defer at the landing).
