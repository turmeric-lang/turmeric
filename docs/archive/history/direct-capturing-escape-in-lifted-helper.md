# Direct backend: capturing `escape`/`call-cc` in an effect handler case miscompiles

**Severity:** low (contrived nesting; a compile error, not a silent miscompile).

**Status:** RESOLVED. Two independent capture walkers each lacked an
`EX_CALLCC` case:

- `collect_free_vars` (`elab_core.c`) -- fixed to descend into a `(call/cc
  f)`/`(escape f)` receiver (both an `EX_CLOSURE`, whose captures are folded,
  and a raw `EX_FN`, whose body free vars are collected excluding its params).
  This threads the escape's enclosing captures into the **shift-body** and
  **perform-continuation** lifted helper envs.
- `collect_handle_captures` (`emit_core.c`) -- fixed to push a callcc receiver
  so its `EX_CLOSURE` case folds the receiver's captures into the fiber
  **handler case** env.

With both walkers complete, all three CT-IR carve-out guards
(`shift_body_ok` / `perform_body_ok` / `handle_case_ok`) were removed, so a
capturing `call/cc`/`escape` in any lifted helper position lowers on DK with
`direct == cps`. Oracles: `cps-oracle-escape-capture-in-shift-body`,
`cps-oracle-escape-capture-after-handle`,
`cps-oracle-escape-capture-in-handler-case`. (An owning-value capture still
bails to the direct emitter -- it is not a Copy capture.)

## Remaining repro (handler case)

```turmeric
(defeffect E [] : int)
(defn g [] : int (perform (E)))
(defn f [n : int] : int
  (handle (g) (E [] k) (resume k (+ 1 (escape (fn [j] (j n)))))))
(defn main [] : int (println (f 5)) 0)
```

```
error: 'n_1271' undeclared (first use in this function)   // in __effect_handler_N
```

The escape receiver captures `n`, but the emitted handler function
`__effect_handler_N` never materializes `n` from its `__env`.

## Root cause

An effect handler case body is emitted as a fiber handler function whose
captures are collected by a **separate** walker, `collect_handle_captures`
(`src/compiler/emit_core.c:3168`) -- not the now-fixed `collect_free_vars`.
`collect_handle_captures` does not descend into an `EX_CALLCC` receiver, so the
escape's capture `n` is absent from the handler's capture set / `__env` and is
undeclared in the generated handler function. (Unlike `collect_free_vars`, this
walker also lacks nested-lambda param exclusion, so descending into the receiver
would need the same care -- fold an `EX_CLOSURE`'s captures, or recurse into a
raw `EX_FN` body excluding its params.)

## Interaction with the CPS backend unification (U2)

The CT-IR backend keeps a guard (`letraw_has_callcc`, used by `handle_case_ok`)
that evicts a callcc-bearing handler case to the direct emitter, so this shape
takes the direct path on both backends (`direct == cps`) rather than diverging.

## Fix directions

- Teach `collect_handle_captures` to descend into an `EX_CALLCC` receiver
  (folding an `EX_CLOSURE`'s captures, or recursing into a raw `EX_FN` body with
  its params excluded), mirroring the `collect_free_vars` fix; then drop the
  `handle_case_ok` guard so the shape lowers on DK directly.
- Or, longer term, retire the direct escape path (U7) and wire the escape
  captures into the CT-IR handler-case helper env.
