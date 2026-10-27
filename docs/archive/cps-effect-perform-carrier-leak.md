# CPS effect lowering leaks per-perform carrier allocations (arg array + Tier-C value boxes)

**Severity:** low (bounded, per-perform/-resume-execution heap leak; not a
correctness bug). Sibling of the fixed DK-node leak
(`docs/archive/cps-delimited-dk-node-leak.md`) -- same family: the effect
lowering heap-allocates a carrier to cross the `intptr`-typed
`dk_perform`/`resume`/handler ABI and never frees it.

> **RESOLVED (2026-07-14).** Both parts are now fixed.
> - **DK-path carriers** (multi-arg `__eargs` array, boxed `dk_perform` args):
>   registered in the per-run reap list via `slot_store_reap`, freed at the
>   outermost entry boundary. Cleared `cps-backend-multiarg-effect`,
>   `-multiarg-effect-float`, `-tierc-effect-arg`.
> - **Fiber-path `tierc-effect`**: resolved as predicted -- by bringing the shape
>   onto the DK path, NOT by patching the fiber runtime. Two gaps were closed
>   (see `docs/upcoming/v1/cps-tier-c-effect-result-native-plan.md`): `fn_sig_ok`
>   never admitted a boxed-aggregate RETURN (so a struct-returning effectful
>   function SIG-REJECTed and evicted to the fiber fallback), and the DK-path
>   Tier-C boxes (resume value + handler-case return + deliver) were never freed.
>   Fix: widen the return gate with `slot_box_ty`; make the per-run reap list the
>   sole owner of every Tier-C box (`slot_store_reap` at every crossing, entry
>   unwrap reads consume=false then reaps). `cps-backend-tierc-effect` now emits
>   natively (`dk_perform`/`dk_handler`) and is leak-clean; marker dropped. Full
>   suite 2178 passed, 0 failed; ASan sweep of all cps fixtures shows no
>   double-frees and no tierc leak.
>
> Historical notes (superseded by the above):
>
> **PARTIALLY RESOLVED (2026-07-12).** The **DK-path** carriers are fixed: the
> multi-arg `__eargs` array and any boxed (Tier-C) argument riding `dk_perform`
> are now registered in the per-run reap list (`__dk_reap_ptr`) and freed at the
> outermost entry boundary, exactly like the DK env structs. A new
> `slot_store_reap` helper (`emit_cps_ir.c`) wraps only the boxes read with
> `slot_load(consume=false)` (shared read-only across a multi-shot resume, so
> never consume-freed at a load site -- reaping them cannot double-free). This
> clears `cps-backend-multiarg-effect`, `-multiarg-effect-float`, and
> `-tierc-effect-arg` (markers dropped).
>
> **Still open:** the Tier-C boxes in `cps-backend-tierc-effect`. This is **not**
> a second, parallel leak to fix in place -- it is the same carrier leak surfacing
> on the **legacy fiber effect fallback**, which the CPS-backend-unification work
> (`docs/upcoming/v2/cps-backend-unification-plan.md`) is folding onto the DK
> substrate. That fixture's `Get` effect returns a **Tier-C** (heap-boxed struct)
> result; a Tier-C effect *result* is not yet in the DK-emittable subset, so the
> whole `run`/`use-get` function is delegated to the **direct emitter**, whose
> effect lowering is the ucontext fiber runtime (`emit_effects.c`:
> `eff_box_expr` boxes the resume value / handler result, `eff_slot_load` never
> frees them). Contrast `cps-backend-tierc-effect-arg`, whose effect has a
> Tier-C *argument* but a scalar result: it stays on the DK `dk_perform` path and
> is fixed above.
>
> The right resolution is therefore **not** to bolt box reclamation onto the fiber
> runtime (and it cannot reuse `__dk_reap_ptr` anyway: the reap prelude is gated on
> DK/CPS usage at `emit_module.c:6967`, so a pure-fiber program would reference an
> undefined symbol). It resolves when a **Tier-C effect result** is brought onto
> the DK effect path (part of the `emit_cps.c` retirement / unification), at which
> point the resume-value and handler-result boxes flow through
> `slot_store` / `dk_perform` and are already covered by the `slot_store_reap`
> treatment added here. `cps-backend-tierc-effect` keeps `requires.no-leak-check`
> until that lands.

## Summary

When an effect crosses the perform/resume boundary, the CPS effect lowering
mallocs a carrier and hands it through the int64 ABI, then never reclaims it:

1. **Multi-arg argument array.** An effect with >1 argument marshals its args
   into a heap `int64_t[]` (`__eargsN`) passed as the single `dk_perform` arg.
   The array is never freed. One leak per perform of a multi-arg effect.

2. **Tier-C value boxes.** A non-scalar effect argument / resume value / handler
   result (a struct or heap-boxed ADT) is boxed with `malloc(sizeof(T))` to ride
   the `intptr` slot (`__bx` / `__eb`). The box is never freed. One leak per
   boxed value crossing the boundary (arg box, resume-value box, handler-return
   box).

## Minimal repros

Multi-arg array (`cps-backend-multiarg-effect`, `-float`):

```turmeric
(defeffect Sum3 [a : int b : int c : int] : int)
(defn add3 [base : int] : int
  (handle
    {(perform (Sum3 1 2 3)) + base}
    (Sum3 [a b c] k) (resume k {{{a * 100} + {b * 10}} + c})))
(defn main [] : int (println (add3 1000)) 0)
```

LSan: `24 byte(s) leaked` from `add3__cps` (the `malloc(3 * sizeof(int64_t))`).

Tier-C value box (`cps-backend-tierc-effect`, `-arg`):

```turmeric
(defstruct Pr [first : int second : int])
(defeffect Get [] : Pr)
(defn use-get [] : Pr (perform (Get)))
(defn run [] : Pr
  (handle (use-get) (Get [] k) (resume k (make-struct Pr :first 32 :second 10))))
(defn main [] : int
  (let [p (run)] (println (+ (.first p) (.second p)))) 0)
```

LSan: two `16 byte(s)` leaks from `__effect_handler_*` (the resume-value box and
the handler-return box); the `-arg` variant leaks the perform-arg box.

## Root cause

- Multi-arg array: `src/compiler/emit_cps_ir.c:3428` in `emit_perform` --
  `int64_t *__eargsN = malloc(n * sizeof(int64_t))`; the array is passed to
  `dk_perform` and never freed. (The 1-arg path passes a scalar, no allocation,
  so it does not leak.)
- Tier-C boxes: `slot_store` boxes non-scalar values at
  `src/compiler/emit_cps_ir.c:204` (`__bx`), and `eff_box_expr` at
  `src/compiler/emit_effects.c:36` (`__eb`), for the perform arg, the resume
  value, and the handler result. None are freed after the value is consumed.

The handler case reads the arg array / unboxes the value once (see the
`__eargs`/`__eb` readback in the lifted handler at `emit_cps_ir.c:2650`), after
which the carrier is dead -- but no free is emitted.

## Fix directions

1. **DONE (DK path).** The `__eargsN` array and any boxed `dk_perform` argument
   are registered with `__dk_reap_ptr` at construction and freed at the outermost
   entry boundary (the per-run reap list from
   `docs/archive/cps-delimited-dk-node-leak.md`). Boundary reaping is safe for a
   multi-shot resume: each perform execution allocates a fresh carrier, registers
   it once, and every replay reads it read-only before the top-level `dk_run`
   settles. Only boxes read with `slot_load(consume=false)` are wrapped
   (`slot_store_reap`), so the entry-return box that `slot_load(consume=true)`
   frees is never double-freed.
2. **OPEN (resolves via unification, not a fiber-side patch).** The remaining
   `cps-backend-tierc-effect` boxes leak because a Tier-C effect *result* forces
   the function onto the legacy fiber effect fallback (`emit_effects.c`). Bring a
   Tier-C effect result into the DK-emittable subset so `handle`/`perform`/`resume`
   lower through `dk_handler`/`dk_perform` (part of the `emit_cps.c` retirement in
   `docs/upcoming/v2/cps-backend-unification-plan.md`); the resume-value and
   handler-result boxes then ride `slot_store` and are picked up by
   `slot_store_reap` with no fiber-specific reclamation. Do **not** add a parallel
   free path to the fiber runtime -- it is being retired, and it cannot reference
   the DK reap list (prelude gated at `emit_module.c:6967`).

## Affected fixtures

- Resolved (marker dropped): `cps-backend-multiarg-effect`,
  `cps-backend-multiarg-effect-float`, `cps-backend-tierc-effect-arg`.
- Open (retains `requires.no-leak-check`): `cps-backend-tierc-effect` (fiber
  path).
