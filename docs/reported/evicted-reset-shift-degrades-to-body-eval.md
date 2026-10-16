# A `reset`/`shift` in a function the CPS backend evicts silently degrades to body-eval

**Severity:** medium-high. The program compiles cleanly and computes the wrong
value. This is a regression of
[direct-reset-shift-degrades-out-of-subset](../archive/history/direct-reset-shift-degrades-out-of-subset.md):
that fix lived in `emit_cps.c`, which was deleted with the direct lowering
(2026-07-12).

## Repro

```turmeric
(defn twice [v : int] : int (* 2 v))

(defn plain [x : int] : int
  (reset (+ 1 (shift twice x))))

(defn with-vec [xs : (Vec int) x : int] : int
  (reset (+ 1 (shift twice x))))

(defn main [] : int
  (println (plain 10))               ;; 20 -- correct: shift is abortive
  (println (with-vec (vec-new) 10))  ;; 21 -- wrong
  0)
```

`TUR_TRACE_EVICT=1 tur emit-c` shows the difference:

```
[EVICT] SIG-REJECT             eff=0 with-vec
```

The only thing that differs is a parameter `fn_sig_ok` refuses. The emitted
C for `with-vec` is the context applied to the receiver, not an abort:

```c
static int64_t with_hyvec(tur_adt_Vec__int * xs, int64_t x) {
        int64_t __t203 = twice(x);
        return (INT64_C(1)) + (__t203);
}
```

## Root cause

A colored function outside the CPS set `S` is emitted by the direct emitter
(`emit_cps_ir.c:emit_cps_ir_try_fn_impl` returns false). Two things then go
wrong:

- `emit_effects.c:emit_effects_reset` evaluates the body
  (`emit_value(ctx, body, e->as.reset_.body)`). Its comment assumes every
  reset that can reach a shift is in `S`.
- `emit_effects.c:emit_effects_shift`/`emit_effects_shift0` apply the receiver.
  They abort only through `emit_shift_escape_abort`, which is gated on
  `ctx->in_shift_escape`, and nothing sets `in_shift_escape` any more. Its
  setter was `emit_cps_reset_escape` in the deleted `emit_cps.c`.

Unlike `perform`, which raises "this effect operation has no lowering here"
in `emit_expr.c:emit_value`, an evicted shift has no guard. Because `eff=0`,
the taint rules in `ensure_S` never see it either.

## Fix directions

- **Cheapest: fail loudly.** In `emit_effects_reset`, when the body reaches a
  base `shift`/`shift0` bound to this reset (`cps_expr_contains_shift`), raise a
  located error like the `perform` one, naming `TUR_TRACE_EVICT`.
- **Restore the escape path.** The runtime half survives: `tur_shift_reset_ctx`
  and `tur_cur_shift_reset` are still emitted in `emit_module.c`. Re-add a
  `setjmp` landing in `emit_effects_reset` and set `ctx->in_shift_escape`
  while the body is emitted, as the deleted `emit_cps_reset_escape` did.
- **Shrink eviction.** Admit the refused signature in `fn_sig_ok`. That fixes
  this repro but not the class: `SIG-EXPORT` and `SIG-INLINE-C` evict too.

Whichever is chosen, re-add a fixture for an evicted function. The
`cps-oracle-*` fixtures only cover functions that stay in `S`.
