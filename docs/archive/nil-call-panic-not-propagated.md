# A panic inside a `: nil` call does not stop its caller

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by
`nil-call-panic-propagates`.

**Severity:** medium. After a call to a `: nil` function panics, the compiled
caller keeps running its next statements before the panic is noticed.

## Repro

```turmeric
(defn boom [n : int] : nil
  (when (> n 0) (panic "boom")))

(defn work [n : int] : int
  (boom n)
  (println "after boom")
  n)

(defn main [] : int
  (let [r (catch-unwind (fn [] (work 1)))]
    (println (if (ok? r) "ok" "caught")))
  0)
```

- `tur --interpret`: `caught`.
- `tur run`: `after boom`, then `caught`.

## Root cause

A call in statement position is normally followed by a `tur_panicking` check
that returns early. For a call whose result type is `nil`,
`src/compiler/emit_expr.c:emit_value` returns before emitting that check, and
`emit_stmt` writes a bare `boom(n);`. The flag stays set and is only noticed
by a later check, after the intervening statements have run.

Not yet checked: whether functions emitted by the CPS backend
(`cps_panic_check`) have the same gap.

## Fix directions

- Emit the panic check after a `nil` call in statement position, the same
  check the value path emits. Keep the elision only where the callee is
  provably non-panicking.

## Resolution (2026-10-17)

Both backends now check the panic signal after a `: nil` call in statement
position, as they already did after a call with a value:

- direct emitter: `emit_stmt` (`src/compiler/emit_stmt.c`) follows the bare
  `boom(n);` with `emit_panic_signal_return`, which fires the function's open
  defer frames and returns;
- CPS backend: the `CT_LETRAW` nil arm (`src/compiler/emit_cps_ir.c`) follows
  the statement with `cps_panic_check` when it is a call. The open question in
  the report was answered by measuring it: the CPS path had the same gap.

The check is unconditional, like the value path's; a callee that cannot panic
costs one predictable branch. The `expected.c` snapshots were regenerated.

Pinned by `nil-call-panic-propagates`, which covers a direct and a CPS-lowered
caller, and checks that a `: nil` call that does not panic still runs on.
