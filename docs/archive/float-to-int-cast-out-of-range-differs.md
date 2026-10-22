# A float cast to an integer type out of its range is a bare C cast on both engines (undefined); the answers differ

**RESOLVED 2026-10-22** -- see [Resolution](#resolution-2026-10-22). Pinned by
`float-to-int-cast-saturates` (every width, NaN, `-0.5`, and run-time
operands, on all three engines).

**Severity: low-medium** (engine divergence on an input C leaves undefined).
Split out of type-forms-gaps item 7 on 2026-10-21. **Decision (2026-10-21):
saturate on both engines.**

## Repro

```turmeric
(defn main [] : int
  (println (as uint8 300.7))    ; 255 compiled, 44 interpreted
  (println (as int8 -129.5))    ; -128 compiled, 127 interpreted
  (println (as int 1e30))       ; INT64_MAX compiled, INT64_MIN interpreted
  0)
```

## Root cause

Converting a float outside the target type's range is undefined behaviour in C,
and neither engine guards it. The compiled program emits a bare cast,
`TUR_AS(uint8_t, 300.7)` = `((uint8_t)(300.7))` (`emit_module.c`); the 255 above
is gcc folding a constant operand, not a rule -- with the value arriving at run
time the same cast can produce anything (on x86-64 typically `INT64_MIN` or the
low bits). The interpreter converts with a plain C cast too (`(int64_t)d`, then
narrows), which yields `INT64_MIN` for an out-of-range double and then wraps.

## Fix

A saturating float-to-integer helper in the runtime preamble (one per target
width, or one returning `int64_t` plus a clamp per width): clamp to the target's
`[MIN, MAX]`, NaN to 0, truncate toward zero inside the range -- Rust's `as`
rule. Emit it for every float-to-integer `as` (keep `TUR_AS` for the in-range
spelling the float-conversion check knows), and call the same rule from the
interpreter's `as`. A preamble change: regenerate the snapshots and the split
runtime. Document the rule in sized-primitives-guide next to the integer wrap it
already describes, and pin the three lines above plus NaN, `-0.5` and a
run-time (non-constant) operand on all three engines -- the run-time operand is
the case the current fixtures cannot see.

## Resolution (2026-10-22)

Saturation, on both engines, as decided:

- **Compiled.** The runtime preamble (next to `TUR_AS`, `emit_module.c`)
  defines `tur_f2i64` / `tur_f2u64` -- NaN to 0, beyond the range to the
  nearest bound, truncation toward zero inside it -- and one clamp per narrow
  width (`tur_f2i8` ... `tur_f2u32`). `(as <integer kind> <float>)` emits the
  helper for its target width (`emit_expr.c`, the `EX_CAST` arm); the
  integer-to-float direction keeps `TUR_AS`. The helpers live in the
  preamble, so `tests/check-emitted-float-conversions.py` attributes the one
  real cast to the runtime, and the fixture is clean under it. The split
  runtime (`src/runtime/generated/`) was regenerated, so `tur jit` saturates
  too.
- **Interpreter.** `EX_CAST` (`src/turi/eval.c`) applies the same rule
  through `turi_f2i_sat` / `turi_f2u_sat`.
- **Documented** in sized-primitives-guide next to the integer wrap.
