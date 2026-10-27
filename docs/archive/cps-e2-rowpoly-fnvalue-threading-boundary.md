# E2: row-polymorphic effect fn-value threading -- sound for direct-under-handler HOFs, unsound for cross-HOF-delegated multi-hop

**STATUS: RESOLVED (commit 4b8ea6b).** The sound landing turned out cleaner than
either route below: relax the row-variable param gate under the flag AND, under
the flag, skip the cross-HOF leaf-fiber delegation in
`colored_call_wbd_delegatable` (cps_ir.c) -- so the caller threads the HOF's
`__cps` through the whole chain (`main -> apply-logged__cps -> apply__cps ->
lambda`) instead of delegating `apply` to fiber.  poly-infer DK-lowers correctly
(no route-1 "leave it on fiber" needed).  Verified: the whole effect family
flag-on == flag-off baseline, real fiber-live 27 -> ~18, default suite 2203/0
(flag-off byte-identical), full flag-on soundness sweep clean.  The two-route
analysis below is retained as the paper trail.

**Severity:** medium (a precise, verified E2 sub-slice boundary; no code landed --
the blanket relaxation is UNSOUND on one fixture, so it must NOT ship as-is).
This report records a deep investigation so the sound version executes without
re-deriving the boundary.

## The finding (measured)

The effect-poly/-row/-subtype SIG-TAINT family is NOT blocked on the fat-closure
`tur_poly_fn_t.fn_cps` channel (E2 sub-slice 1, landed).  It is blocked on the
**row-polymorphic effect fn-value param** gate in `param_thread_class`
(src/compiler/emit_cps_ir.c:3282):

```c
if (p->type.kind == TY_FN) {
    const struct EffectRow *pr = p->type.as.fn.effect_row;
    if (!pr || pr->kind != ERK_CONCRETE) return PT_E1;   /* row-poly -> evict */
}
```

A `(fn [] #fx{e} int)` param (row variable) tiers PT_E1 -> the effectful lambda
flowing in is `sig_perm` fiber -> its handler co-classifies SIG-TAINT.  These
lambdas ARE carried as bare int64 fn-ptrs (not `tur_poly_fn_t`), so the E2a
REGISTRY path (`via_registry` `__tur_cps_lookup(f)(args, __kont)`) is the right
channel -- the gate just refuses it for a row variable.

**Relaxing the gate (`if (0 && ...)`) is SOUND for 4 fixtures** -- verified
byte-correct output, zero `eff=1`:

- `effect-poly-typeclass` (2), `effect-poly-bracket` (release),
  `effect-row-ho` (42), `effect-row-compose` (42).

These share a shape: the HOF (`run-twice` etc.) is called DIRECTLY from the
handler-installing fn (`main`), so threading `__kont` from the HOF's fn-value call
reaches the handler.

## Why the blanket relaxation is UNSOUND (the one break)

`effect-poly-infer` **aborts** with the relaxation.  Root cause, pinned:

```turmeric
(defn apply [f : (fn [int] #fx{e} int) x : int] #fx{e} : int (f x))     ; HOF
(defn apply-logged [x : int] #fx{Log} : int (apply (fn [v] (do (perform (Log "calling")) v)) x))
(defn main [] : int (handle (do (apply-logged 5) 0) (Log [msg] k) (do (println msg) (resume k nil))))
```

There is an INTERMEDIATE wrapper: `main` (handler) -> `apply-logged` -> `apply` ->
lambda.  `apply-logged__cps` emits `apply(...)` -- the **direct/fiber entry** --
then `dk_run(__kont, result)`, NOT `apply__cps(..., __kont)`.  So the lambda's
`Log` perform runs on the fiber with no handler in scope (main's handler is on the
DK) -> escapes -> abort.

The delegation is deliberate: `colored_call_wbd_delegatable`
(src/passes/cps_ir.c:684-700) explicitly delegates `apply-logged = (apply callback
x)` to the fiber when `apply` indirect-calls a fn-value param and a
concrete-effectful fn-value arg is passed -- *"the taint model keeps the effect
fiber ... so the whole cluster stays consistently on the fiber runtime."*  That
co-classification invariant ASSUMES the callback (lambda) is fiber.  The row-poly
relaxation threads the lambda onto the DK while `apply` stays fiber-delegated ->
the performer/handler split the two machines -> the escape.

Confirmed with a single-level variant (`e2single`: the same lambda-with-arg + HOF
called DIRECTLY in the handler fn, no `apply-logged` wrapper) -> prints `calling`
correctly.  So the break is purely the cross-HOF-delegated multi-hop, not the
lambda-arg or data-arg HOF.

## Sound landing (two routes, pick one)

1. **Threadability guard (smaller).**  Make `fn_value_threadable`
   (emit_cps_ir.c) return false for a lambda whose value-use flows into a HOF call
   that `colored_call_wbd_delegatable` (cps_ir.c) would delegate.  Then
   poly-infer's lambda stays fiber (consistent with `apply` being delegated) and
   the 4 direct-under-handler wins still thread.  Obstacle: the two predicates are
   in different translation units (emit_cps_ir.c vs cps_ir.c) -- needs a shared
   "this call whole-body-delegates" query, or the delegation decision surfaced
   into the threadability walk.  This is the recommended first slice: it lands the
   4 wins soundly and leaves poly-infer correctly on the fiber.
2. **E1 multi-hop threading (larger).**  Give a colored fn (`apply-logged`) a
   `cps->cps` call to a colored HOF (`apply`) that passes a fn-value ARG across the
   `__cps` boundary (E1 non-scalar-signature threading).  Then the whole chain
   `main -> apply-logged__cps -> apply__cps -> lambda` threads `__kont` and
   poly-infer DK-lowers.  Removes the delegation entirely; the biggest reward but
   entangled with E1.

## Verification recipe for the next attempt

- Relax gate at emit_cps_ir.c:3282 + apply route 1 or 2.  Target: the 4 wins emit
  their HOF `__cps` with zero `eff=1` and correct output; `effect-poly-infer`
  EITHER DK-lowers (route 2) OR stays SIG-TAINT fiber with output `calling` (route
  1) -- never aborts.
- Flag-off byte-identical (this is flag-gated admission surface).
- FULL flag-on soundness sweep (every effect fixture: flag-on output == baseline)
  -- this family is exactly where the earlier E2 attempt was reverted for
  unsoundness, so the sweep is mandatory, not optional.

## Context

The effect-poly/-row/-subtype/-ho SIG-TAINT cluster in
docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md Stage E.  Complements E2
sub-slice 1 (the `tur_poly_fn_t.fn_cps` ABI slot, landed): that slot is for the
FAT/capturing effectful fn-value; THIS cluster is the row-poly BARE-fn-ptr case
that threads via the E2a registry once the row-variable gate is relaxed behind the
soundness guard above.
