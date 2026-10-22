# Compiled: an effect capture cannot cross a `catch-unwind`

**Severity: medium.** A compile-time refusal, not a wrong answer, and
`tur --interpret` runs every shape below. It **blocks
[discontinue-plan](../upcoming/discontinue-plan.md)** on the compiled engine:
the plan's reason for keeping `discontinue` is that a `catch-unwind` in the
body, around the `perform`, can catch the raise and carry on, and today no
compiled program can put a `catch-unwind` between a `perform` and its handler.
Found 2026-10-22 checking what blocks that plan; reproduces on `main` (`a0a718f3`) and on #1326's head.

## Repro

```turmeric
(defeffect Ask [] : int)
(defn body [] : int
  (let [r (catch-unwind (fn [] : int (+ 1 (if (> (perform (Ask)) 0) (panic "boom") 0))))]
    (if (err? r) 100 200)))
(defn main [] : int
  (println (handle (body) (Ask [] k) (resume k 5)))
  0)
```

| | `tur run` / `tur jit` | `tur --interpret` |
| --- | --- | --- |
| `perform` inside the thunk (above) | refused: `this effect operation has no lowering here`, plus ``note: `Ask` escapes this function value`` | `100` |
| the same, with the `perform` in a helper the thunk calls | refused the same way | `100` |
| `catch-unwind` in the CLAUSE around `(resume k 5)` | refused: `no lowering here` on the body's `perform` (`TUR_TRACE_EVICT=1`: `BODY-UNSUPPORTED main unsupported form: EX_CATCH_UNWIND`) | `100` |

All three engines also warn, wrongly, `TUR-W0033: handler clause for 'Ask' is
unreachable: the body does not perform 'Ask'` on the first two shapes.

What does work: a `catch-unwind` in a function that holds neither the
`perform` nor the `handle`, around a call that does both
(`tests/fixtures/panic-in-handler`, `defer-cps-caught-panic`).

## Root cause

- **CPS IR.** `EX_CATCH_UNWIND` / `EX_CATCH_PANIC_OF` have no rule in
  `src/passes/cps_ir.c` (they fall to "unsupported form", `cps_ir.c:586`), so a
  function holding one is evicted to the direct emitter, which lowers
  `catch-unwind` as a setjmp landing on the C stack. A `handle` in that
  function then has no lowering.
- **The thunk.** The thunk is a function value handed to the direct-emitted
  landing, and a call through it starts with no handler in scope
  (`emit_expr.c:18322`, the "escapes this function value" note). It is the same
  structural gap as
  [compiled-shift-delimited-by-root](compiled-shift-delimited-by-root.md): a
  direct-emitted delimiter around a CPS callee.
- **TUR-W0033.** `collect_effects_in_expr` (`src/passes/effect_check.c:674`)
  walks the thunk as an expression; the thunk is a `fn` literal, whose effects
  are latent, so the `perform` in its body is never counted against the
  enclosing `handle`. `catch-unwind` calls its thunk at once, so the thunk's
  body effects belong to the `catch-unwind` node.

## Fix directions

1. **A `catch-unwind` term in the CPS IR.** Lower it as a DK frame that marks a
   panic boundary, so the thunk's body is translated inline (it is called
   exactly once, at once) and a `perform` inside it captures through the
   boundary like any other frame. The DK machine already has the pieces: CPS
   frames return on `tur_panicking` (`cps_panic_check`) and abandoned frames
   fire their defers (`__dk_panic_abandon`). The boundary frame consumes
   `tur_panicking` and delivers `(Err payload)`. A multishot copy of a chain
   holding the boundary copies the boundary too.
2. **TUR-W0033 alone (small):** in `effect_check.c`, when the thunk of
   `EX_CATCH_UNWIND` / `EX_CATCH_PANIC_OF` is a `fn` literal, collect its body.
   That fixes the false warning on every engine, independent of (1).
