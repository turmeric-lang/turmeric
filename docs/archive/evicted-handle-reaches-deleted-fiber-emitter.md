# An evicted `handle` with no reachable `perform` emits C for the deleted fiber runtime

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by
`evicted-handle-unreachable-clauses` and `errors/evicted-handle-over-perform`.

**Severity:** low. The build fails with raw cc errors instead of a located
diagnostic, and only when nothing under the `handle` can perform.

## Repro

```turmeric
(defeffect Ask [] : int)

(defn quiet [xs : (Vec int)] : int
  (handle 5
    (Ask [] k) (resume k 41)))

(defn main [] : int
  (println (quiet (vec-new)))
  0)
```

`tur run` warns TUR-W0033 (the clause is unreachable), then cc fails:

```
error: unknown type name 'TurEffectCaptureCtx'
error: 'FiberBlock' has no member named 'eff_ctx'
error: '__outer_cap' undeclared ...
```

`TUR_TRACE_EVICT=1` shows `[EVICT] SIG-REJECT eff=1 quiet`: the `(Vec int)`
parameter fails `fn_sig_ok`.

## Root cause

An evicted colored function is emitted by the direct emitter, and its
`EX_HANDLE` goes to `emit_effects.c:emit_effects_handle`. Apart from the
`(unsafe ...)` marker short-circuit at its top, that function is still the
fiber-era lowering. It references `TurEffectCaptureCtx` and `FiberBlock::eff_ctx`,
which were deleted with the fiber effect runtime on 2026-07-19 (see the comment
in `emit_module.c` above the `FiberBlock` definition).

When the handled body can perform, directly or through a callback, the
`perform` raises the located "this effect operation has no lowering here"
error first (`emit_expr.c:emit_value`), so cc never runs. A `handle` whose body
performs nothing is the shape that gets through.

A side issue: that located error lists only loop shapes as known causes. Its
common trigger is a signature `fn_sig_ok` refuses (`SIG-REJECT`), or the
performer being tainted by one (`SIG-TAINT`), and the text does not mention
that.

## Fix directions

- In `emit_effects_handle`, when the handle is not a pure-`Unsafe` marker,
  either:
  - emit the body directly when no clause is reachable (W0033 already proves
    this), or
  - raise the same located error as `perform`.

  Then delete the fiber body. It cannot produce compilable C any more.
- Mention `SIG-REJECT`/`SIG-TAINT` in the `emit_value` error text, or print the
  evicting function's `[EVICT]` category in the error itself.

## Resolution (2026-10-17)

The fiber-era body of `emit_effects_handle` (`src/compiler/emit_effects.c`) is
deleted. A `handle` that reaches the direct emitter now takes one of three
paths:

- **Inert** (`emit_handle_is_inert`): the `(unsafe ...)` marker, or a handle
  none of whose clauses the body can reach. `check_unreachable_handlers_in_expr`
  (`src/passes/effect_check.c`) records the second case on the new
  `HandleExpr.no_clause_reachable` when it issues TUR-W0033 for every clause.
  Nothing under such a handle suspends, so its body is emitted in place, in
  value position and (through `emit_stmt`) in statement position.
- **A `perform` under it:** each one reports the existing located "this effect
  operation has no lowering here" error at its own site, as the fiber walk used
  to, so the list of known shapes still points at the real cause.
- **Neither:** a located error at the handle itself (`emit_handle_no_lowering`).

The side issue is fixed too: the `perform` error (and the new handle error)
now say what SIG-REJECT and SIG-TAINT mean.

Pinned by `evicted-handle-unreachable-clauses` (both positions run and print)
and `errors/evicted-handle-over-perform` (`requires.compiled`).
