# `(k 2.25)` into an `escape-cont<float32>` passes the double's bits; the landing reads 0

**RESOLVED 2026-10-22** -- see [Resolution](#resolution-2026-10-22). Pinned by
`escape-float-through-k` (`e5`..`e7`: a float literal, `7.1`, and a `float`
value into a `float32` continuation, on all three engines).

**Severity: low** (wrong answer; needs a `float32` continuation and a float
literal). Found 2026-10-21 fixing the float landing of `call/cc`
(continuation-forms-gaps, archived as cps-reset-continuation-joins-and-escape-float).

## Repro

```turmeric
(defn e5 [] : float32 (call/cc (fn [k : escape-cont<float32>] (k 2.25))))
(defn main [] : int (println (e5)) 0)
```

`tur run`: `0`. `tur --interpret`: `2.25`. With a `float32`-typed argument
(`(k x)`, `x : float32`) both print `2.25` (pinned by `escape-float-through-k`).

## Root cause

`(k v)` on a continuation lowers to `tur_escape_resume(k, <v as an int64 word>)`
(`elab_call.c`, the CONT_ESCAPE arm, through `call_wrap_reinterpret`). The literal
`2.25` is a `float` (double), and the argument check accepts it for a `float32`
continuation without converting it, so the word carries the double's 64 bits.
The landing reads a `float32` from the low 32 bits of that word (`emit_cps_callcc`,
`emit_cps_ir.c`), which for 2.25 are zero.

## Fix

Convert `v` to the continuation's declared element type before the reinterpret
(`(as float32 v)` semantics for a float literal or `float` value), in the same
arm. The same applies to the cloneable and serial resume arms if they accept a
narrower element type. Pin `(k 2.25)` and `(k 7.1)` into a `float32` continuation
on all three engines.

## Resolution (2026-10-22)

The `(k v)` sugar's CONT_ESCAPE arm (`elab_call.c`) now converts the resume
value to the continuation's declared element type before the int64
reinterpret, when that type is a float kind and the value is a numeric of
another kind: an `EX_CAST` to `float32`, exactly the `(as float32 v)` the
program would otherwise have had to spell. `escape-cont<T>` records `T` as
the continuation's `returns` (its `arg` is unknown, which is why the
resume-value check never fired), and for an escape continuation the value
delivered IS the landing's result, so `returns` is the type to convert to.
Integer-kind targets are left alone: an int literal already took `TUR_AS`
through the same cast, and a float into an `escape-cont<int>` is a type error
to make loudly, not a silent truncation.

The cloneable and serial arms were not changed: their `returns` is what
`(k v)` yields, not what it accepts, so there is no declared element type to
convert to there.
