# A function that performs and takes a wide by-value aggregate parameter is refused

**Severity: low-medium.** A correct program is refused at compile time, never
miscompiled. But nothing in the program points at the cause: the error lands
on the `perform` ("this effect operation has no lowering here"), and every
function sharing the effect is evicted with it (`SIG-TAINT` / `BODY-TAINT`).
Found 2026-10-13 while fixing
[adt-handle-result-as-call-arg-evicts](../archive/adt-handle-result-as-call-arg-evicts.md).

## Repro

```turmeric
(defdata Shape (Circle :int) (Rect :int :int))
(defeffect Ask [] int)
(defn area2 [s : Shape] : int
  (+ (perform (Ask)) (match s (Circle r) (* 3 r) (Rect w h) (* w h))))
(defn s5 [] : int (handle (area2 (Rect 2 4)) (Ask [] k) (resume k 3)))
(defn main [] : nil (println (s5)))
```

| | result |
| --- | --- |
| `tur --interpret` | `11` |
| `tur run` | "this effect operation has no lowering here" at the `perform` |
| `Shape` narrowed to fit 16 bytes (`(Rect :int)`) | compiles |

`TUR_TRACE_EVICT=1`: `SIG-REJECT eff=1 area2`, then `SIG-TAINT eff=1 s5`.

## Root cause

`fn_byval_agg_param_ok` (`src/compiler/emit_cps_ir.c`) admits a by-value
aggregate parameter only when the direct emitter ALSO passes it by value,
`!type_struct_pass_by_ptr(p->type)`. Its comment gives the reason: `emit_params`
spells the `__cps` parameter by value, so a by-pointer one made the entry
wrapper disagree with the direct forward decl (`const T *`) -- a `conflicting
types` error.

That reason may no longer hold. The entry wrapper was taught the by-pointer
spelling on 2026-07-22
([sepcomp-cps-entry-wrapper-byptr-agg-param-mismatch](../archive/sepcomp-cps-entry-wrapper-byptr-agg-param-mismatch.md)):
`cps_entry_param_by_ptr` / `cps_entry_param_byptr_ctype` declare the wrapper's
parameter `const T *`, matching the forward decl, and dereference it into the
by-value `__cps` call. The plan that added the exclusion
(`docs/archive/cps-dk-sole-effect-lowering-plan.md`, E1 by-value param slice)
predates that fix.

## Fix directions

1. Lift the `type_struct_pass_by_ptr` exclusion in `fn_byval_agg_param_ok` and
   see what breaks. The pieces that have to agree:
   - the forward decl and the entry wrapper (`const T *`; done since 07-22);
   - the `__cps` body (by value);
   - a cps->cps caller (passes the atom by value; already what
     `atoms_csv_call_typed` does with `to_direct == false`);
   - a cps->direct caller of the wrapper (passes `&(atom)`; done by the fix
     that found this report).

   Check both whole-program and `--shared` builds; the 07-22 bug was a
   separate-compilation one.
2. If that is not enough, the parameter could cross the `__cps` boundary as
   `const T *` too. That is a larger ABI change.
