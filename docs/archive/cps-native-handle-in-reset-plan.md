---
title: "CPS backend -- native emission of a handle nested in a reset (KK_PROMPT-delivering handle continuation)"
status: BOTH REDUCTIONS LANDED. Reduction A (receiver-free handle-in-reset) and Reduction B (cross-function shift, incl. capturing receiver + single/multi-shot) both emit natively and pass their oracles (106; 11/105/23/306). Memory-safe under ASan; a snapshot-not-freed leak remains (same class the fiber path had, far smaller) -- tracked as a follow-up, fixture carries requires.no-leak-check. Full suite 2179/0.
description: A `handle` whose continuation delivers into an enclosing `reset`'s prompt (KK_PROMPT) is evicted from the CPS backend to the direct/fiber emitter, where its effect boxes leak. Cross-function `shift` is one instance -- its `__Shift` desugar produces exactly this handle-in-reset nesting. This plan is the authoritative, self-contained reference: it records the reproductions, the exact failing predicates, the proven-feasible mechanism (the `__dk_cont_fn` bridge), the coordinated change, and -- critically -- the dead ends already ruled out, so the next agent does not re-derive them.
---

## TL;DR (read this first)

- **The gap is NOT `shift`-specific and NOT receiver-capture-specific.** It is
  **native emission of a `handle` nested inside a `reset`** whose continuation
  delivers to the enclosing reset's prompt (`KK_PROMPT`). Cross-function `shift`
  is merely one instance: its `__Shift` desugar produces exactly that nesting.
- **Proven by reduction (decisive):** a plain handle-in-reset with *no shift at
  all* -- `(reset (+ 100 (handle (use-e) (E [] k) (resume k 5))))` -- also evicts
  (`BODY-STRUCT-OR-TAINT`, zero `__cps`). The same `handle` *not* inside a reset
  is native (`cps-backend-effect`). So the missing capability is the nesting, full
  stop.
- **It is feasible, not architecturally blocked.** The two continuation machines
  (DK chains and `tur_cloneable_cont`) already interoperate at exactly the needed
  seam via `__dk_cont_fn` (`emit_cps_cloneable_bridge_prelude`). The crux is one
  new codegen site (bridge-wrap the DK subk before the dynamic receiver call); the
  rest are admission-predicate widenings.
- **It cannot land as isolated slices.** The admission predicates are COUPLED --
  each independently evicts, so no single-predicate change is both non-inert and
  non-regressing. It must land as ONE coordinated, oracle-gated change.
- **Do not repeat the dead ends** in the section of that name below. Several
  "obvious" first moves (general `atom_ok` widening, effect-free scoping,
  `is_delegatable_capturing_closure`, `cps_shift_body_kf`) were each tried and
  each miscompiles or is inert. They are recorded so you skip them.

## Status update (BOTH reductions landed)

Both reductions are native on this branch. What actually landed, precisely, so the
next agent does not re-derive it:

### Reduction A -- receiver-free handle-in-reset (native)

The single admission gap was that a `handle` reached inside a reset's delimited
body fell through `delim_ok`'s default case to `term_core_ok`, whose
`reset_body_ok(handle.body)` rejects the KK_PROMPT delivery of the handle's
continuation. Fix: a dedicated `CT_HANDLE` case in `delim_ok`
(src/compiler/emit_cps_ir.c) that admits BOTH the handled body and the
continuation via `delim_ok` (both deliver to a prompt: the handled body to the
handle's own prompt, the continuation to the enclosing reset's prompt), with the
cases on `handle_case_ok`. No new codegen -- `emit_handle` already lifts
`handle.body` as an `LH_RESET_CONT` frame delivering through `cur_k`. `use-e`/`wrap`
emit `__cps`, print `106`, ASan/LSan-clean. Fixture:
`tests/fixtures/cps-backend-handle-in-reset/`.

### Reduction B -- cross-function shift (native, incl. capturing receiver)

Landed as the exact coordinated change the plan predicted, plus one correction to
the "dead ends":

1. **`delim_ok` CT_HANDLE also checks `handle.delim` via `delim_ok`** (not
   `term_core_ok`). Reduction B's handled body `(+ 10 (inner))` builds a
   `letcont j` heap-join whose jbody delivers `(+ 10 t)` to the prompt (KK_PROMPT);
   term_core_ok rejects that, delim_ok admits it. (Reduction A's plain
   `tailcall use-e(<prompt>)` delim also passes delim_ok, so this is a widening.)
2. **`atom_ok` for the `__Shift` receiver, effect-scoped at the CT_PERFORM site.**
   The receiver rides the perform as a `TY_FN` atom (`CA_VAR`/`CA_CVAR`).
   `shift_recv_atom_ok` admits it ONLY when `is_shift_effect(perform.effect)` --
   never via general `atom_ok` (the dead-end miscompile).
3. **Bridge-wrap emit crux (the only genuinely new codegen).** In `emit_lifted`
   LH_HANDLER_CASE, when `hcase->effect` is `__Shift`, the continuation `k` is
   bound as `tur_cloneable_cont_alloc(__dk_cont_fn, dk_copy_range(subk, NULL),
   __dk_env_clone, __dk_env_drop)` instead of the raw `DK *k = subk`. The receiver
   `(recv k)` then resumes via `tur_cloneable_cont_resume(tur_continuation_snapshot
   (k), v)` -> per-resume clone of the DK chain -> `dk_invoke`. Bridge prelude gate
   in emit_module.c widened to `cps_uses_cloneable_rt && dk_machine_emitted`.
4. **Capturing receiver (step) -- DEAD END #3 IS RESOLVED by the bridge.** A
   capturing `(fn [k] (k base))` receiver is delegated (built via CT_LETRAW) by a
   new `Closure.is_shift_receiver` flag set at the __Shift desugar (elab_effects.c)
   and honored in `is_delegatable_value` (cps_ir.c). It is invoked only as
   `(recv k)` in the bridge-wrapping handler case (never indirect-called), so the
   `indirect_callee_ok` hazard does not apply. Scalar captures ride the lifted env
   via collect_caps. `is_shift_receiver` MUST be explicitly zero-initialized at all
   three Closure alloc sites -- arena memory is not zeroed.
5. **`emit_letraw` casts a `TY_FN` binder store through `(int64_t)(intptr_t)`** so
   the receiver fat-fn value stops emitting a -Wint-conversion pointer->int store.

All of inner/outer/step/run/twice/thrice/sum2/sum3 emit `__cps`; `direct == cps ==`
`11`/`105`/`23`/`306` (single- and multi-shot); full suite 2179/0.

**Leak (deferred, tracked).** The native path is memory-SAFE under ASan (no UAF /
double-free / UB) but LEAKS ~3320 bytes on the fixture: `tur_continuation_snapshot`
clones the DK chain per resume and nothing frees the snapshot -- the SAME
snapshot-not-freed leak the fiber path already had (fiber leaked ~4 MB here; the
native path leaks far less). Fixing it needs resume-drop discipline in the shared
direct-emitter receiver codegen -- out of this change's scope. The fixture
`tests/fixtures/shift-crossfn-resume-works/` carries `requires.no-leak-check`; see
`docs/reported/cps-resume-frame-node-leak.md`.

## Reproductions (two oracles)

### Reduction A -- plain handle-in-reset (the SIMPLER oracle; build this first)

```turmeric
(defeffect E [] : int)
(defn use-e [] : int (+ 1 (perform (E))))
(defn wrap [] : int
  (reset (+ 100 (handle (use-e) (E [] k) (resume k 5)))))
(defn main [] : int (println (wrap)) 0)   ; expects 106
```

`wrap` evicts today: `TUR_TRACE_EVICT=1 tur emit-c` shows `BODY-STRUCT-OR-TAINT`
and the emitted C has zero `wrap__cps` / `use-e__cps`. Remove the enclosing
`(reset (+ 100 ...))` and the bare `(handle (use-e) (E [] k) (resume k 5))` is
native (that is `cps-backend-effect`). This is the smallest thing that must go
native; gate the core change on it FIRST -- it has no receiver/`__Shift`
complications.

### Reduction B -- cross-function shift (the downstream beneficiary)

`tests/fixtures/shift-crossfn-resume-works`:

```turmeric
(defn inner  [] : int (shift (fn [k : cont] (k 1)) 0))                      ; resuming, no capture
(defn twice  [] : int (shift (fn [k : cont] (+ (k 1) (k 2))) 0))            ; multi-shot, no capture
(defn thrice [] : int (shift (fn [k : cont] (+ (+ (k 1) (k 2)) (k 3))) 0))  ; multi-shot, no capture
(defn step   [base] : int (shift (fn [k : cont] (k base)) 0))               ; resuming, CAPTURES base
(defn run    [] : int ... asserts direct == cps == 11 / 105 / 23 / 306 ...)
```

Oracle values: `11`, `105`, `23`, `306` (single- AND multi-shot). Today `step`
evicts `BODY-UNSUPPORTED (EX_CLOSURE (capturing closure))` and `inner`/`twice`/
`thrice`/`run` evict `BODY-STRUCT-OR-TAINT` -- **all** of them fall to the
direct/fiber emitter, not just the capturing `step`. (The "inner/twice/thrice are
already native" claim in older drafts was a misread of `--dump-cps`, which prints
the *built* IR, not whether the function EMITS a `*__cps`. Corrected -- see Dead
Ends.)

## Diagnostics -- how to get ground truth

- **`TUR_TRACE_EVICT=1`** (built-in): prints one line per colored function that
  evicts, with the category: `SIG-EXPORT`, `SIG-MAIN`, `SIG-REJECT`,
  `BODY-UNSUPPORTED` (+ the unsupported form), `BODY-STRUCT-OR-TAINT`. This is the
  first thing to run; it tells you which functions and which gate.
- **`--dump-cps`**: prints the BUILT colored IR. USEFUL for seeing the desugared
  shape (`perform __Shift(...)`, the `handle`/`reset` nesting), USELESS for
  deciding nativeness -- a function can dump clean IR and still evict at emit.
  Confirm nativeness by grepping the emitted C for `<name>__cps`.
- **`TUR_TRACE_TAINT` / `TUR_TRACE_TCO`** (NOT built-in -- must be re-added):
  temporary `fprintf(stderr, ...)` probes inside the taint fixpoint (`ensure_S`)
  and `term_core_ok`. They gave the definitive root-cause map below. Re-add them
  when you resume; they are the only way to see WHY `term_core_ok` returns false
  per node (it has many early-return points).

## Exact failing-predicate map (instrumented, definitive)

The taint is a **consequence, not the cause**: every user of the effect
independently fails `term_core_ok` (seed `core_ok=0`), so each is a genuine fiber
function and they mutually taint the effect. Per node:

- **Performer** (`use-e`; or `inner` = `perform __Shift(recv)`):
  `CT_PERFORM` fails `atom_ok` on its argument. For `__Shift` the arg is a `TY_FN`
  receiver value, which `atom_ok` admits via neither `slot_ty` nor
  `carrier_handle_ok` (heap-ADT only). (`body_ok=1 reset_ok=1 args_ok=0`.)
- **Handler** (`wrap`; or `outer`/`run` = `(handle BODY (E [] k) CASE)` nested in
  a reset): `CT_HANDLE` fails BOTH:
  - `term_core_ok(handle.delim)` -- the handled body calls the performer; and
  - `reset_body_ok(handle.body)` -- the handle's CONTINUATION. Its `CT_APPCONT`
    delivers to the enclosing reset's prompt, and **`term_core_ok`'s `CT_APPCONT`
    case returns false for `KK_PROMPT` delivery**. THIS is the core gap. (`xslot=1
    delim=0 body_ok=0`.)
  - The handler CASE (`(recv k)` for `__Shift`, `(resume k 5)` for plain) needs
    `handle_case_ok`; for `__Shift` it is a dynamic application of the receiver
    `TY_FN` value to the continuation.

The predicates are **COUPLED**: the handler's `term_core_ok(delim)` fails partly
because `delim` calls the not-yet-admissible performer -- a circular dependency
with the performer's own admission. No node goes native until the whole cluster
does.

## Feasibility -- RESOLVED: feasible via the DK<->cloneable bridge

The one machine-coherence worry (a DK subk cannot be resumed through the
receiver's `tur_cloneable_cont_resume`) is already solved in-tree:

- The `__Shift` receiver (e.g. `__fn_1282` = `(fn [k] (k 1))`) is COLORED but
  emitted only as a direct entry `static int64_t __fn_1282(int64_t k)` that
  resumes via `tur_cloneable_cont_resume(tur_continuation_snapshot(k), 1)`.
  `(recv k)` calls it dynamically by fn-pointer value.
- `emit_cps_cloneable_bridge_prelude` (src/compiler/emit_dk_runtime.c) already
  emits **`__dk_cont_fn`** -- a `tur_cloneable_cont` whose env is a DK chain and
  whose resume dispatches to `dk_invoke`, with clone=`__dk_env_clone`
  (=`dk_copy_range`) and drop=`__dk_env_drop`. So a DK subk WRAPPED via
  `tur_cloneable_cont_alloc(__dk_cont_fn, dk_copy_range(subk,NULL), __dk_env_clone,
  __dk_env_drop)` resumes correctly through the receiver's existing
  `tur_cloneable_cont_resume`. **The two continuation machines interoperate at this
  exact seam.** Multi-shot (`twice`/`thrice`) gets an independent snapshot per
  resume from `__dk_env_clone` -- verify under ASan.

## The coordinated change (complete map -- all must land together)

1. **`term_core_ok` `CT_APPCONT` / `reset_body_ok`**: admit a `KK_PROMPT`-
   delivering handle continuation (the core gap; drives Reduction A). Also admit
   the heap-join `delim` for a body that calls a performer.
2. **`atom_ok`**: admit the `__Shift` receiver atom. MUST be `__Shift`-SCOPED, not
   a general `TY_FN` widening and not "effect-free `TY_FN`" (both proven to
   miscompile -- see Dead Ends). Scope by testing the enclosing
   `CT_PERFORM`/`CT_HANDLE` effect == `__Shift`, never atoms/effects in general.
3. **`fn_sig_ok`** for performer + handler + receiver (the receiver's
   `cont -> int` param / `CT_RESUME` body must be admitted).
4. **`handle_case_ok`** for the case body (`(resume k 5)` plain; the dynamic
   `(recv k)` for `__Shift`).
5. **New emit (the crux, only genuinely new codegen):** in the DK handler case,
   BRIDGE-WRAP `subk` via `__dk_cont_fn` before the dynamic `(recv k)` call, so the
   receiver's `tur_cloneable_cont_resume` resumes the DK chain. Everything else is
   admission-predicate widening.
6. **Leaks**: Tier-C boxes on this path already go through `slot_store_reap` (from
   the landed Tier-C-effect-result work); confirm coverage under ASan.

Land order suggestion: get Reduction A green with items 1/3/4 (+ its emit) FIRST
-- it has no receiver, so it isolates the `KK_PROMPT`-continuation core change from
the `__Shift` scoping. Then layer items 2 + 5 for Reduction B.

## Dead ends -- DO NOT REPEAT (each was tried; each fails)

1. **General `atom_ok` TY_FN widening.** Admitting any bare `TY_FN` atom
   MISCOMPILES `cps-backend-effectful-callback`, `effect-row-ho`,
   `effect-poly-typeclass` (stdout mismatch -- wrong values, NOT a build/snapshot
   change, so it slips past snapshot regen). `fn_sig_ok` admits effectful fn params
   ON PURPOSE, relying on taint to keep the callback's handler co-fiber; a general
   widening flips the handler to DK while the callback still performs on fiber ->
   split machines -> wrong output.
2. **"Effect-free `TY_FN`" scoping of `atom_ok`.** Does NOT work: `effect_row` on
   a lambda-lifted callback's type is ERASED (`a->type->as.fn.effect_row == NULL`
   for `(fn [] (perform (Ask)))`), so an effect-free check wrongly admits it ->
   `tur: unhandled effect (tag 2)` abort (third miscompile). The scoping MUST be
   by effect identity (`== __Shift`), not by the atom's own effect row.
3. **~~`is_delegatable_capturing_closure` (the receiver-capture idea).~~ RESOLVED
   -- this now WORKS, because the bridge (item 3 above) is in place.** The original
   failure ("delegated `(recv k)` resume routed through fiber and tainted __Shift")
   was a PRE-BRIDGE artifact: without the LH_HANDLER_CASE bridge-wrap, the capturing
   receiver's `(recv k)` resumed a raw DK subk through the fiber cloneable path. With
   the bridge, `k` is a bridge-wrapped `tur_cloneable_cont`, so a capturing receiver
   resumes the DK chain like a non-capturing one. As landed, a capturing receiver is
   delegated via a scoped `Closure.is_shift_receiver` flag (NOT a blanket
   capturing-closure delegation, which is still unsafe -- `indirect_callee_ok`).
   `step`/`run` now emit `__cps` and produce `105`. The order the plan gave was
   right: the capture only works AFTER items 1-3; it is not the gap on its own.
4. **`cps_shift_body_kf` (`(recv val)` synthesis / beta-reduce).** Instrumentation
   proves it is NEVER called for the cross-function shift path -- it is dead code
   for this shape. The mechanism is the `__Shift` desugar in `elab_effects.c`, not
   `emit_shift`/`cps_shift_body_kf`.
5. **Reading `--dump-cps` as a nativeness oracle.** It prints built IR, not
   emission. "It dumps `perform __Shift`, so it's native" is false. Confirm with
   `TUR_TRACE_EVICT` + grepping emitted C for `__cps`.

Also note: this is NOT a taint bug to be fixed in the taint fixpoint. Taint is
downstream of the `term_core_ok` failures; fix those and the taint clears.

## Oracles / gates (do not land without ALL green)

- **Reduction A** prints `106`, and `wrap`/`use-e` emit `__cps` (no eviction).
- **Reduction B** (`shift-crossfn-resume-works`) keeps `direct == cps` = `11` /
  `105` / `23` / `306`, single- AND multi-shot, with `step`/`inner`/`twice`/
  `thrice`/`run` all emitting `__cps`.
- **Effectful-callback set** stays green: `cps-backend-effectful-callback`,
  `effect-row-ho`, `effect-poly-typeclass` (these are what the `atom_ok` widening
  breaks -- they are the regression tripwire).
- **ASan sweep** clean on the above: no leak, no double-free, no use-after-free.
  (Multi-shot must give each resume an independent snapshot via `__dk_env_clone`.)
- Full suite (`bash tests/run.sh`, 12-min / 720000ms timeout) + snapshots
  regenerated in the same change.

## Key files / symbols index

- `src/compiler/emit_cps_ir.c` -- `atom_ok`, `fn_sig_ok`, `sig_slot_ok`,
  `term_core_ok`, `reset_body_ok`, `delim_ok`, `handle_case_ok`, `shift_body_ok`,
  `perform_body_ok`, `emit_handle`, `emit_reset`, `emit_shift`, `emit_perform`,
  `emit_resume`, `emit_deliver_ty`; the taint fixpoint `ensure_S` (`SEnt`, `in_s`,
  `perf_lo/hi`, `hand_lo/hi`, `base_taint`).
- `src/compiler/emit_dk_runtime.c` -- `emit_cps_cloneable_bridge_prelude`,
  `__dk_cont_fn`, `__dk_env_clone` (=`dk_copy_range`), `__dk_env_drop`; the
  `__dk_reap_*` per-run reap registry.
- `src/compiler/elab_effects.c` -- `wrap_reset_body_with_shift_handler` (:559,
  wraps `B -> (handle B (__Shift [recv] k) (recv k))`), the
  `(shift RECV BODY) -> (perform (__Shift RECV))` desugar (:745),
  `elab_get_shift_effect`.
- `src/passes/cps_ir.c` -- `is_delegatable_value` (:377, admits closures with
  `n_captures==0`), `indirect_callee_ok` (:402), `build_handle`, `build_perform`,
  `cps_bind`, `cps_tail`, `cps_shift_body_kf` (:1116, DEAD for this path).
- Runtime: `tur_cloneable_cont_resume`, `tur_cloneable_cont_alloc`,
  `tur_continuation_snapshot`, `dk_invoke`, `dk_copy_range` (cps_prompt.c,
  fiber effect runtime).

## Non-goals

- Non-capturing resuming / multi-shot receivers are ALREADY native only under the
  belief corrected above -- in fact they evict too, so they ARE in scope (Reduction
  B). Do not treat them as done.
- Owning-field (rc/ref/weak) captures in the receiver -- gate item 4 /
  env-capture-owning-values; out of scope, bails via `collect_caps` non-Copy guard.
- The `dk_frame_resume` per-resume node leak (multi-shot) -- separate finding
  (`docs/reported/cps-resume-frame-node-leak.md`); persists on the native path and
  may keep `shift-crossfn-resume-works`'s `requires.no-leak-check` marker until
  both are done.
- Deleting the general direct/fiber fallback (N6.5 / Task 2) -- this is one input.
