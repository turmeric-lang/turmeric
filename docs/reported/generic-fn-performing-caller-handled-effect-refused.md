# A generic function that performs an effect its caller handles is refused

**Severity: medium (expressiveness).** A correct program is refused at compile
time, never miscompiled. Any function with a type parameter that `perform`s an
effect handled outside its own body fails with "this effect operation has no
lowering here". This holds at every instantiation, `int` included, and every
function sharing the effect is evicted with it. The interpreter runs it. Found
2026-10-13 while probing
[cps-performing-fn-with-wide-adt-param-refused](../archive/cps-performing-fn-with-wide-adt-param-refused.md);
it does not depend on aggregate width.

## Repro

This is the example
[cps-backend-generic-monomorph-classification-plan](../archive/cps-backend-generic-monomorph-classification-plan.md)
gives as "verified sound" (`direct == cps == 10`):

```turmeric
(defeffect A [] :int)
(defn emit-twice [T] [x : T] : int { (perform (A)) + (perform (A)) })
(defn run [] : int (handle (emit-twice 0) (A [] k) (resume k 5)))
(defn main [] : nil (println (run)))
```

| | result |
| --- | --- |
| `tur --interpret` | `10` |
| `tur run` | "this effect operation has no lowering here" at the first `perform` |
| `(defn pass-through [T] [x : T] : T (do (perform (A)) x))` at `int`, a 16-byte ADT, a 24-byte ADT | the same, all three |
| implicit type variable (`[x : a]`) | the same |

`TUR_TRACE_EVICT=1`: `SIG-REJECT eff=1 emit-twice`.

## Root cause (partly measured)

The plan's soundness argument was that a generic which cannot be CPS-classified
runs its effect on the fiber machine, and the taint fixpoint co-classifies its
handler to fiber too. The fiber effect runtime has since been retired, so the
fallback now has nowhere to go and the `perform` is refused instead.

On the CPS side, the generic template `SIG-REJECT`s on its type-variable
parameter (`fn_sig_ok`). The G3b route for a generic is the mono-template
(`mono_template_all_admissible`, `src/compiler/emit_cps_ir.c`). It stands in
only when every ABI clone (`ctx->abi_specializations` with `spec->fn == fd`)
passes `mono_sig_ok` and the core check, and it answers false when there is no
clone at all. A scalar instantiation is likely called through the int64
carrier with no clone (`emit_abi_note_carrier_call`), so `any` stays false.
Why the aggregate instantiations miss too is not measured. `is_cps_island`
admits only a generic that handles its own effects (`cps-backend-generic-island`).

## Fix directions

1. Let a carrier-called generic's base function be its own CPS entry: its
   signature is the uniform int64 carrier, so a `__cps` twin over carrier
   words is ABI-consistent where a concrete one is not. That is the
   `sig_slot_ok` name-collision hazard `fn_single_concrete_sig` guards
   against, from the other side.
2. Or force an ABI clone for every instantiation of a generic that performs
   an escaping effect, so the mono-template route has something to admit.
   Then find what stops the aggregate clones (trace `mono_sig_ok` /
   `term_core_ok` under `TUR_TRACE_CORE=1` with the clone active).
