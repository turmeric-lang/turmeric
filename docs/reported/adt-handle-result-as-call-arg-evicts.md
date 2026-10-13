# A wide-ADT-valued `handle` passed straight to a direct callee fails to compile

**Severity: low.** A correct program is refused at compile time, never
miscompiled, and binding the `handle` with `let` first is a one-line
workaround. Found 2026-10-13 while fixing
[effect-cont-aggregate-result-untyped](../archive/effect-cont-aggregate-result-untyped.md).
It reproduces on `main` without that change.

## Repro

```turmeric
(defdata Shape (Circle :int) (Rect :int :int))
(defeffect Ask [] int)
(defn area [s : Shape] : int (match s (Circle r) (* 3 r) (Rect w h) (* w h)))
(defn s4 [] : int (area (handle (Rect (perform (Ask)) 4) (Ask [] k) (resume k 3))))
(defn main [] : nil (println (s4)))
```

| | result |
| --- | --- |
| `tur --interpret` | `12` |
| `tur run` | "this effect operation has no lowering here" at the `perform` |
| `(let [s (handle ...)] (area s))` | `12` |
| the same with a struct `P`, `(sum (handle (P ...) ...))` | `7` |
| the ADT `handle` as a `match` scrutinee | `12` |

`TUR_TRACE_EVICT=1` names `s4` as `BODY-STRUCT-CORE`. Every other function
that shares the effect then evicts too, as `BODY-TAINT`. So in a larger
program the error lands on many `perform`s that are fine by themselves.

## Root cause

`TUR_TRACE_CORE=1` fails on the call. `area` is uncolored, so it is a
cps->direct call, and `call_arg_ok` (`src/compiler/emit_cps_ir.c`) refuses a
by-value ADT atom there: the direct emitter's C signature takes it through the
carrier ABI. `cps_direct_param_takes_agg_raw` admits the case where the
callee declares the same aggregate C type, but only for a narrow ADT. A wide
one (`Rect :int :int`) is passed by pointer on the direct path
(`type_struct_pass_by_ptr`), which the raw emission does not match.

## Fix directions

1. In `cps_direct_param_takes_agg_raw`, admit a wide ADT too, and have
   `atoms_csv_call_typed` pass `&atom` for a parameter
   `type_struct_pass_by_ptr` says is by pointer.
2. Or let the CPS translation bind a `handle` call argument to a `let` first,
   which already compiles.
