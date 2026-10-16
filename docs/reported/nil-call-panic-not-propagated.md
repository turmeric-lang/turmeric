# A panic inside a `: nil` call does not stop its caller

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
