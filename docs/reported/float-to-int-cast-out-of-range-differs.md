# A float cast to an integer type out of its range is a bare C cast on both engines (undefined); the answers differ

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
