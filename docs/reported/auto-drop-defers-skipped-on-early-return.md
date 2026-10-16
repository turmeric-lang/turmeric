# Injected scope-exit drops are skipped by an early `return`

**Severity:** medium (leak). Every early `return` out of a `let` that owns a
`ref`, an `rc` or an owning field leaks that binding, once per call.

## Repro

```turmeric
(defn early [flag : bool] : int
  (let [r (ref 5)]
    (if flag
      (return (deref r))
      0)))

(defn main [] : int
  (println (early false))
  (println (early true))
  0)
```

Built and run the way `tests/run-leak-check.sh` does (`-fsanitize=address`,
`TUR_RUNTIME=source`):

- `(early false)` alone: clean.
- With `(early true)`: `SUMMARY: AddressSanitizer: 8 byte(s) leaked in 1
  allocation(s)`, the `ref` box.

The `rc` and rc-field variants do not show under LSan, because the rc runtime
keeps its own blocks. `gc-live-blocks` grows by one per call instead.

`tests/fixtures/rc-auto-drop-early-return` says the return fires the drop. It
does not, and the fixture's output cannot tell the difference.

## Root cause

`src/compiler/elab_forms.c:elab_let` appends the injected drop `defer`s
**after** the body. `src/compiler/emit_expr.c:emit_do_value` registers a
`defer` only when control reaches it in sequence, so a `return` from inside
the body leaves before any of them is registered.

## Fix directions

- Register the injected drop defers immediately after the bindings, before
  the body, so the existing defer-on-return machinery fires them; or
- have the early-return path fire the pending injected drops of every
  enclosing `let`.
- Turn `rc-auto-drop-early-return` into a real check (`gc-live-blocks`
  before and after), and add the `ref` repro under `requires.leak-check`.
