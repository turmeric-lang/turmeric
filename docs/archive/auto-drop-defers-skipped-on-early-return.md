# Injected scope-exit drops are skipped by an early `return`

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by
`ref-auto-drop-early-return` (leak-checked) and `rc-auto-drop-early-return`.

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

## Resolution (2026-10-21)

Fix direction 2: the early-return path runs the enclosing lets' pending
injected drops. Direction 1 (register them before the body) was not taken: a
defer's env captures its variables by value when it is registered, so an `rc`
binding the body reassigns with `set!` would have been released at its first
value -- already released by `elab_set_rc_release` -- and its last leaked.

- Each do-emitter -- `emit_do_value` (both frame paths), `emit_stmt`'s `EX_DO`
  arm and `emit_tail_drop_hoist_do` -- notes the trailing run of `is_drop_glue`
  defers against its frame (`emit_frame_note_pending_drops`, a side table in
  `emit_core.c` keyed like `emit_frame_note_parent`) while it emits the items
  ahead of it, and clears the note when it pushes them.
- The `return` emitter (`emit_stmt`, `EX_RETURN`) runs every noted drop along
  its frame chain, innermost first, inline (`emit_frame_run_pending_drops`).
  The drop reads the binding's C local at the return, so a `set!` in the body
  is respected. A binding the return value moves out -- `(return x)`, or `x`
  as a field of the struct returned -- keeps its drop off that path: the rc
  case returned a live handle before only because the drop was skipped.
- The value is now computed BEFORE anything runs on the way out: the frame
  chain (user `defer`s), the pending drops, the `any` scope drops, then the
  `return`. It used to fire the frame chain first, so a written `defer` ran
  ahead of the expression it was deferred past -- the interpreter, and a
  scope's normal exit, always computed the value first -- and with the drops
  on this path `(return (deref r))` would have read a freed box.

Keyed by frame, the note is invisible to a lambda body (it starts with no
frame), so a `return` inside a lambda drops only the lambda's own lets.

Not covered: a `throw` or a panic out of the body still skips the pending
drops (they leave through the frame chain alone), as the declined-shapes
table in emitted-code-ownership-guide says.

Pinned by `ref-auto-drop-early-return` (the repro, leak-checked, plus the
value-before-defer order on both engines) and `rc-auto-drop-early-return`
(now counts `gc-live-blocks` around flat and nested early returns: 3 blocks
leaked before, 0 after; `requires.compiled`, since `gc-live-blocks` is inline
C). `tests/run-leak-check.sh`: 233 passed, 0 failed, 4 known-open.
