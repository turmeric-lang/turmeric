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
`EX_CALLCC` -- it prints the message above (src/compiler/emit_expr.c, the
`EX_CALLCC` arm) and emits nothing for the expression.  The build still
runs cc on the rest, so the program may link and run with the escape
silently missing, or fail to compile, depending on what surrounded it.

## Fix directions

- Lift an `escape` / `call/cc` that appears in a direct-only function into a
  synthesized CPS helper (the body of the escape site as a zero-arg
  colored thunk, called through the ordinary direct->cps entry wrapper),
  the way a `main` with effects is already entered through d2b.
- At minimum, make the arm a real `TUR-E` diagnostic that names the
  workaround and stops the build, instead of a message on stderr and an
  empty expression.
