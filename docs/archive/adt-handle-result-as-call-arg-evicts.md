# A wide-ADT-valued `handle` passed straight to a direct callee fails to compile

**RESOLVED 2026-10-13 -- fix direction 1.** The repro prints `12` compiled.

- **The gate.** `cps_direct_param_takes_agg_raw` (`src/compiler/emit_cps_ir.c`)
  is now a thin wrapper over `cps_direct_agg_arg_mode`. That function answers
  RAW for a narrow aggregate, as before, and ADDR for a wide one
  (`type_struct_pass_by_ptr`: over 16 bytes, tag word plus widest variant).
  ADDR is answered only when the callee's direct signature really is
  `const T *`: not a closure, not inline C (`cps_entry_param_by_ptr`'s
  exceptions), and the FnDef is in hand. The usual callee is UNCOLORED, so it
  has no CPS entry, and the FnDef comes from `fd_for_binding(g_prog, ...)`,
  as `callee_effect_free` finds it. Missing that lookup was why a first cut
  still evicted.
- **The renderer.** `atoms_csv_call_typed(_offs)` takes a `to_direct` flag
  (`true` from `CT_LETCALL` and the cps->direct tail arm, `false` from the two
  cps->cps sites). On a direct call it passes an ADDR argument as `&(atom)`,
  the direct emitter's own `area(&s)`. The atom is a C local of the frame that
  makes the call, which is synchronous, so the address outlives the use. A
  `__cps` entry takes such a parameter BY VALUE (since
  [cps-performing-fn-with-wide-adt-param-refused](cps-performing-fn-with-wide-adt-param-refused.md)
  admitted it; before that `fn_byval_agg_param_ok` refused it), so the
  cps->cps sites never need the address.
- Pinned by `tests/fixtures/cps-wide-adt-handle-call-arg` (leak-checked). It
  covers the call in tail position, under `+` (`CT_LETCALL`), a wide
  `defstruct` between two scalar arguments, a 16-byte float product that stays
  by value, and a sibling function sharing the effect that the `BODY-TAINT`
  cascade used to take down. Both engines print the same answers. With the
  ADDR answer disabled, the fixture fails to build again.

Found on the way and filed separately:
[cps-performing-fn-with-wide-adt-param-refused](cps-performing-fn-with-wide-adt-param-refused.md)
(since resolved).
A function that performs AND takes a wide aggregate parameter is refused
outright (`SIG-REJECT`).

**Severity (at filing): low.** A correct program is refused at compile time, never
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
