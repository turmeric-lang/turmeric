# `(k 2.25)` into an `escape-cont<float32>` passes the double's bits; the landing reads 0

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
