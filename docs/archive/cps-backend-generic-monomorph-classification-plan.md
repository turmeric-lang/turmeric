---
title: CPS backend -- classify colored-generic monomorphs
status: done
description: The last large N6 fallback lever. A colored *generic* (polymorphic) function is CPS-classified only as its generic template, which sig-rejects on a TY_APP-with-tyvar param/return; the concrete monomorphs the direct emitter actually generates (choose_or__spec__...) are never CPS-emitted, so any DK effect chain that touches one collapses to the fiber machine. This doc scopes the architecture, the soundness situation, the real value, and a phased implementation path.
---

# CPS backend -- classify colored-generic monomorphs

## The gap

A colored **generic** function -- one with a type parameter whose signature
carries a `TY_APP`-with-tyvar param or return (`(defn f [A] [x : (Option A)] :
A ...)`) -- is CPS-classified only as its **generic template**. That template
sig-rejects (`fn_sig_ok`: a `TY_APP` with a free type variable is neither a
scalar, a by-value box, nor a heap carrier), so the function falls back.

The concrete **monomorphs** the direct emitter actually generates and calls
(`choose_or__spec__int64_t_tur_adt_Option__int_int64_t(tur_adt_Option__int o,
int64_t dflt)`) -- whose params *are* concrete by-value ADTs that would pass
`fn_sig_ok` -- are never seen by the CPS classifier. They run on the **fiber**
effect machine (`tur_fiber_block_new` / `EffectHandlerFrame`), never DK.

### Why it is the largest raw lever, and why the raw count overstates it

`sig-param TY_APP` measured ~4352 (plus `sig-ret TY_APP` ~625), the biggest
single fallback bucket. But:

- A **concrete** `TY_APP` (`(Option int)`, `(Pair int int)`, `(Result int
  int)`) already CPS-emits -- it monomorphizes to a by-value owning-free ADT and
  rides the Tier C box path. Verified `direct == cps`.
- The remaining surface is **generic templates** -- the count is inflated by
  per-fixture stdlib re-emission, and much of it is generics whose monomorphs
  never enter a DK effect chain at all (a self-contained combinator that both
  performs and handles its own effect is already correct on fiber).

So the *real* value is narrower than 4352: it is **letting a DK effect chain
include generic monomorphs** -- a colored generic that performs an effect handled
by a CPS peer (or vice versa) -- instead of the whole chain collapsing to fiber.

## Current behavior is SOUND (do not treat this as a bug)

The whole-program effect co-classification in `ensure_S` (the taint fixpoint)
keeps this correct today: a generic monomorph performs on the fiber machine, and
any CPS peer that shares that effect is **evicted** from S (Rule B taint), so a
performer and its handler are never split across the DK and fiber machines. This
was verified:

```turmeric
(defeffect A [] : int)
(defn emit-twice [T] [x : T] : int { (perform (A)) + (perform (A)) })  ; generic -> fiber monomorph
(defn run [] : int (handle (emit-twice 0) (A [] k) (resume k 5)))      ; concrete handler
```

`run` is evicted from CPS (effect A tainted by the fiber monomorph) and the whole
chain runs on fiber -- `direct == cps == 10`. The gap is **coverage, not
correctness**: these functions never get the DK treatment, but they never
miscompile.

## Why this is not a `fn_sig_ok` widening (the architectural crux)

The two passes run at different times over different representations:

| Pass | When | Sees |
| --- | --- | --- |
| CPS classification (`ensure_S`) | a whole-program **pre-emit** walk of `program->as.program.items` | top-level `EX_FN_DEF` **generic templates** + concrete fns |
| Monomorphization | **lazily during emit**, as the direct emitter walks call sites (`emit_abi_intern_spec` interns an `EmitAbiSpecialization` per `(generic, arg_types, result_type)`) | concrete monomorphs, born as `EmitAbiSpecialization`, never as `FnDef` program items |

So when `ensure_S` runs, the monomorphs **do not exist yet**. There is nothing
concrete to classify. Widening `fn_sig_ok` to accept a tyvar `TY_APP` param would
only admit the *generic template* (carrier-ABI, int64) -- but nothing calls the
generic template; every call site is rewritten to a concrete monomorph
(`emit_abi_register_call` records the call->spec mapping). A CPS'd generic
template would be dead code.

## The one existing hook point

`emit_fn_def` (emit_fns.c) is re-invoked for **each** monomorph body with
`ctx->current_abi_specialization` set to that spec (`use_abi_spec =
ctx->current_abi_specialization->fn == fd`). It calls `emit_cps_ir_try_fn`
(emit_fns.c:2613) at the top -- so the CPS backend **is** entered once per
monomorph. Today `emit_cps_ir_try_fn` ignores the active specialization: it
looks the generic `fd` up in the `ensure_S` cache (which sig-rejected) and
returns false, falling through to the direct spec emit.

That is the seam to widen: when a specialization is active, translate the generic
body **under the spec's type substitution** into a monomorph CTerm, classify
*that*, and -- if it lands in the subset -- emit `<spec->clone_name>__cps` + the
entry wrapper, routing the call site to it.

## The hard part: whole-program co-classification over monomorphs

`emit_cps_ir_try_fn` per-monomorph gets the *body* emitted, but S-membership is
not a per-function decision -- it is a **whole-program fixpoint** (Rule A: drop
heap-join-needing candidates; Rule B: evict any S function sharing a tainted
effect). That fixpoint currently enumerates only the program's top-level
`FnDef`s. To admit monomorphs it must enumerate them too, which means their
effect sets must be known **before** the per-monomorph emit -- but monomorphs are
discovered lazily *during* emit.

Two ways to break the ordering knot:

1. **Pre-emit monomorph discovery.** Add a pre-pass (before `ensure_S`'s
   fixpoint) that walks call sites and interns the same `(generic, arg_types,
   result_type)` keys the direct emitter's `emit_abi_intern_spec` would, so
   `ensure_S` can enumerate every monomorph, translate each under substitution,
   collect its effect set, and run the taint fixpoint over the full monomorph
   set. Emit-time then reuses the decisions. This duplicates the direct
   emitter's spec-discovery logic (or refactors it to run once, up front, shared
   by both).
2. **Two-phase, conservative.** Keep classification keyed on generics but treat a
   colored generic as *tentatively CPS-able*; at monomorph-emit time, if all
   effects the monomorph touches are still in S (not tainted by any fiber peer),
   emit the DK version, else fall back per-monomorph. Risk: a monomorph emitted
   early could commit to DK before a later fiber peer taints the shared effect,
   breaking the invariant -- so this needs the effect taint to be resolved
   up front regardless, which collapses back into (1)'s discovery pass.

(1) is the honest design. The cost is a shared monomorph-discovery pass; the
payoff is that both the direct emitter and the CPS classifier work from one
enumerated spec set.

## Phased plan

- **G1 -- translate a generic body under substitution + report admissibility.
  LANDED (analysis-only).** Rather than deep-copy the body with substituted
  types, the probe translates the generic body as-is (`cps_ir_translate_fn`) and
  makes the *gate* substitution-aware: a `g_cps_mono_resolver` hook (an `EmitCtx
  *`, non-NULL only during the probe) is consulted by `slot_ok_t`, which resolves
  each type through the active `EmitAbiSpecialization` (`emit_resolve_type`)
  before gating. So the existing `fn_sig_ok` / `term_core_ok` run **unchanged**
  but see the monomorph's concrete types. Wired behind `--dump-cps-mono`
  (`g_dump_cps_mono`), fired from `emit_cps_ir_try_fn` when
  `ctx->current_abi_specialization` pins a colored generic; prints
  `[cps-mono] <clone_name>: sig=.. body=.. => ADMISSIBLE|fallback`. The
  `slot_ok_t` hook is a strict no-op when the flag is off (full suite: 2037
  passed, 0 failed -- zero emit change). Verified: `choose-or` (`(Option A)`
  param), `wrapit` (`(Option A)` return), and `firstish` (`(Pair A B)` param) all
  report **ADMISSIBLE** at their `int` monomorphs -- the substitution-translation
  works for the stdlib-parametric-ADT surface (the bulk of `sig-param TY_APP`).

  *Finding for G2 (resolver completeness).* A **user-defstruct** generic
  (`(defstruct Box [A] (val A) (tag :int))`, `boxup : (Box A)`) reports a **false
  `fallback`**: its *concrete* analog (a `defstruct BoxI [val tag]` monomorphic
  fn) CPS-emits fine, but `emit_resolve_type` resolves `(Box A)` to a `(Box int)`
  `TY_APP` **without attaching the monomorphized `AdtDef`**, so `slot_box_ty`
  cannot see it is an owning-free by-value product. Stdlib ADTs dodge this
  because their monomorphs are pre-registered. G2/G3 must materialize the full
  monomorph type (the direct emitter's `emit_abi_instantiate_type` + monomorph
  ADT registration), not just `emit_resolve_type`, to admit user-defstruct
  generics. The probe is honest about this: it *under*-reports (never claims a
  fallback monomorph is admissible), so it is a safe lower bound on the surface.

  Only a scalar-tyvar generic (`f [T] [x : T]`, `T`=int) has no distinct
  `__spec__` clone -- it emits once as the int64-carrier base generic, so the
  probe (which keys on an active specialization) does not cover it; that is a
  separate carrier-generic sub-case, not part of the `TY_APP` monomorph surface.

- **G2 -- accurate monomorph classification + discovery-timing findings.**
  *Signature classification via materialized spec types LANDED (analysis-only).*
  The probe now reads the direct emitter's MATERIALIZED concrete types
  (`spec->result_type` / `spec->arg_types[]`) for the signature (`mono_sig_ok`)
  instead of re-resolving the generic annotation. This closed the G1
  false-negative half-way: `emit_resolve_type` can collapse `(Box A)` to the bare,
  unapplied `TY_ADT Box` the by-value-product gate rejects (`byval_prod=0`), while
  the spec's `result_type` is the fully-applied `TY_APP (Box int)` the gate
  accepts (`byval_prod=1`, same `AdtDef`). Verified: `boxup`'s user-defstruct
  return flips `sig=no` -> `sig=ok`; the stdlib cases stay ADMISSIBLE; full suite
  2037/0 (the resolver hook + `mono_sig_ok` are used ONLY in the dump probe --
  zero emit change).

  *Remaining G2/G3 blockers, now pinned down:*

  1. **Body-internal resolution of user-defstruct results.** `boxup` still reports
     `body=no`: the generic body's `(make-struct Box ...)` result is elaborated as
     a **bare `TY_ADT Box`** (no type arg), so `emit_resolve_type` -- pure tyvar
     substitution -- has nothing to substitute and the by-value gate rejects it.
     Stdlib constructors (`some`, `pair`) return an annotated `(Option A)` /
     `(Pair A B)`, which is why their bodies resolve. Fixing this needs the
     direct emitter's full materialization (`emit_abi_instantiate_type` + the
     ADT's own `[A]` type-param scope), not just `emit_resolve_type`. The probe
     only ever *under*-reports, so it stays a safe lower bound.

  2. **The monomorph set is not complete when `ensure_S` runs (the ordering
     knot).** `ensure_S` first fires inside the main function-emission loop
     (emit_module.c ~9217, via the first colored fn's `emit_cps_ir_try_fn`), but
     `emit_abi_register_call` interns specs *incrementally throughout* that emit
     (and later scan passes at ~9410/9505). So a later function's monomorphs do
     not exist yet when `ensure_S` builds its S-set and runs the effect-taint
     fixpoint. Making the fixpoint range over monomorphs therefore requires a
     **dedicated pre-emit discovery pass** -- run the full `emit_abi_scan_expr`
     over the program to intern every spec *before* `ensure_S` -- or an
     equivalent reordering. This is the load-bearing refactor for G3 and is
     confirmed unavoidable.

- **G3 -- emit the monomorph DK body. Investigated; two findings reshape it.**

  *Finding A (major de-risk): the CPS emit path is ALREADY spec-aware.* The plan
  feared G3 would have to thread type substitution through the whole CPS emitter.
  It does not: the CPS type-spelling helpers (`binder_ctype_full`, `slot_store`,
  `slot_load`, `emit_params`) all spell aggregate types via `emit_type_c_name`,
  which is `type_c_name(emit_resolve_type(ctx, t))` -- and during the spec-body
  emit loop `ctx->current_abi_specialization` is set, so those spellings already
  resolve `(Box A)` -> `tur_adt_Box__int`. So a monomorph body reuses the
  generic CTerm as-is (the forms are identical; only the type *spellings* differ,
  and they resolve at emit). The emit plumbing is essentially ready.

  *Finding B (the real remaining blocker): routing vs. the safety gate.* There
  are two ways to reach a monomorph's DK body, with very different risk:

  - **No routing needed for a self-contained island.** If a monomorph is emitted
    as `<clone_name>__cps` + a `<clone_name>` entry wrapper (the wrapper installs
    its own `dk_prompt` root), callers keep calling the SAME `<clone_name>(args)`
    symbol -- no call-site routing, no whole-program taint change. But this is
    only *sound* when the monomorph is a genuine island: every effect it performs
    is handled within its own body (nothing escapes to a caller -- an escaping
    `dk_perform` would search only the wrapper's fresh root and never reach the
    caller's handler), AND it calls no colored function (else a fiber callee's
    perform would miss this body's DK handler). Getting that **self-contained +
    no-colored-callee** gate wrong is a silent runtime miscompile, so it needs a
    validated escaping-effect analysis over the CTerm (performed-effects not
    lexically enclosed by a matching handle = escaping; require empty), plus a
    colored-callee check -- not yet built.

  - **Cross-function DK chains need the taint surgery.** The valuable case (a
    generic monomorph that performs an effect handled by a CPS peer) still
    requires `ensure_S`'s taint fixpoint to range over the complete monomorph set
    (the pre-emit discovery pass) AND to stop letting the non-emitted generic
    *template* taint its own effect once its monomorphs are candidates.

  So G3 splits: **G3a** = self-contained-island emit (bounded, but gated on a
  new validated escaping-effect analysis); **G3b** = cross-function chains (the
  pre-emit discovery + taint surgery). Neither is a rush job -- G3a's safety gate
  and G3b's taint surgery are both correctness-critical.

- **G3a -- self-contained-island monomorph DK emit LANDED.** A colored-generic
  monomorph that is a self-contained island now CPS-emits (DK) instead of fiber.

  *The safety gate.* `is_cps_island(term)` -- no perform escapes a matching
  enclosing handle, no colored callee (`binding_is_colored`), no raw reset/shift
  -- validated both ways: self-contained (`choose-or`, `wrapit`, `firstish`) ->
  island=yes; escaping perform handled by a caller, a case body calling a colored
  fn, "handle A but perform B" -> island=no. Reported by `--dump-cps-mono` as
  `ISLAND-EMITTABLE` vs `ADMISSIBLE(cross-fn)`.

  *The emit.* When `emit_cps_ir_try_fn` runs for a monomorph
  (`ctx->current_abi_specialization` pins the generic) that is admissible
  (`mono_sig_ok` + `term_core_ok`) and an island, it emits `<clone>__cps` + a
  `<clone>` entry wrapper (installing its own `dk_prompt` root), reusing the
  generic CTerm. Callers keep calling the SAME `<clone>(args)` symbol -- no
  whole-program taint change, no call-site routing. The reused CTerm's type
  spellings resolve to the concrete monomorph types via the active spec; scalar
  tyvars resolve through `binder_ctype_full` (extended for `TY_TYVAR`); floats and
  boxes tier at the concrete type via a `cps_resolve_ty` hook in
  `slot_store`/`slot_load`/`slot_box_ty`; `ce.ret_ty` / wrapper return =
  `spec->result_type`. All of that is a strict no-op when the hook is off (every
  ordinary emit path), so existing output is byte-identical.

  *The carrier-crossing fix (the blocker a first attempt hit).* The delimited
  continuation captures `o : (Option A)` (a by-value `tur_adt_Option__int`) into
  the handler-frame env, then the delegated `(unwrap-or o dflt)` -- a carrier-ABI
  generic call -- was emitted by the direct emitter as a carrier->by-value
  reconstruction `(tur_option_t *)(intptr_t)(o)`, casting the aggregate to
  `intptr_t`. Root cause: the fiber `emit_params` (emit_fns.c) sets
  `param->emit_byvalue_carrier_abi = true` on a concrete by-value ADT-app spec
  param so the delegated call passes it by value, but the CPS `emit_params` did
  not. Fix: the island-emit path sets that flag on each such param (mirroring
  emit_fns.c), so `(unwrap-or o dflt)` now emits `unwrap_or__spec__(o, dflt)` --
  by value, no crossing.

  *Result.* `choose-or`/`wrapit`/`firstish` monomorphs CPS-emit with 0 fiber
  markers and round-trip (`direct == cps`). A float instantiation is CPS-correct
  (`7.5`) where the fiber path is not (a pre-existing float-through-effect bug).
  Fixture `cps-backend-generic-island`; full suite 2038 passed, 0 failed.

- **G3b -- cross-function DK chains LANDED.** A colored generic monomorph that
  PERFORMS an effect handled by a CPS peer (e.g. `ask-plus@int` performs `Ask`,
  `run` handles it) now runs the whole chain on DK. The five requirements the
  earlier pass identified all resolved more cheaply than feared:

  1. **Pre-emit discovery is FREE.** The feared ordering knot does not exist:
     `emit_abi_scan_program` (emit_module.c ~8802) runs a full-program spec scan
     *before* the main emit loop, so `ctx->abi_specializations` is already
     complete when `ensure_S` first runs. `ensure_S` just needed the ctx
     (`g_emit_ctx`, set in `emit_cps_ir_try_fn`; `g_ents_ctx` invalidates the
     cache if a prior `program_has_emittable` classified under a NULL ctx).
  2. **Taint surgery = mono-template.** A colored generic template whose EVERY
     monomorph spec is CPS-admissible (`mono_template_all_admissible`:
     `mono_sig_ok` + `term_core_ok` per spec) is flagged `mono_template` and does
     NOT taint its effects -- its monomorphs stand in for it. The fixpoint evicts
     it (reverting to tainting) if a genuine fiber peer taints its effect. The
     "all monomorphs admissible" condition makes the tag-level taint sound (every
     instantiation is DK), so the per-instantiation problem dissolves.
  3. **Emit.** A non-island monomorph of a surviving mono-template emits
     `<clone>__cps` + wrapper (the G3a machinery, condition widened from
     `island` to `island || se->mono_template`).
  4. **Routing.** A cps->cps `CT_TAILCALL` to a mono-template callee resolves the
     call site to the monomorph clone (`find_mono_clone_for_call`: match a spec by
     callee binding + concrete arg-type C-spelling; NULL if 0 or >1 match, never
     guess) and threads `<clone>__cps(args, k)`. Each mono-template monomorph's
     `<clone>__cps` is forward-declared (a DK caller references it before the
     spec-body loop defines it).
  5. **Wrapper hazard is a non-issue.** The type system forbids an uncolored
     caller from calling an unhandled-effect function, so a non-island monomorph's
     entry wrapper is only ever reached from a context that handles the effect;
     colored callers route to `__cps`, so the wrapper is typically dead.

  Verified: `ask_plus@int__cps` performs `dk_perform(Ask)`, and `run__cps` calls
  `ask_plus__spec__..._cps(tag, base, __h0)` threading its own handler prompt --
  a genuine two-function DK chain, one end a generic monomorph. The result
  depends on the effect (`5 + 100 = 105`), so a machine split would give a wrong
  value. Fixture `cps-backend-generic-cross-fn`. Full suite: 2039 passed, 0
  failed (the taint change over every colored generic regressed nothing).

  *Residual:* `find_mono_clone_for_call` returns NULL for an ambiguous call (all
  args untyped scalars at >1 monomorph); that call then falls to cps->direct on
  the unresolved generic name -> a link error (loud, not silent), acceptable
  under the experimental flag. No fixture hits it.

- **G4 -- widen `fn_sig_ok` for the concrete monomorph types.** With monomorphs
  classified under their concrete param/return types, the tyvar-`TY_APP`
  rejection no longer applies to them (they are concrete by-value ADTs / heap
  carriers, already admitted). The generic *template* still sig-rejects and stays
  dead -- that is correct.

## Status / recommendation

**G1-G3b all landed.** The colored-generic-monomorph lever is essentially
complete: a colored generic monomorph now CPS-emits (DK) whether it is a
self-contained island (G3a) or participates in a cross-function effect chain with
a CPS peer (G3b). The earlier-feared blockers dissolved:

- **G1** (analysis probe) + **G2** (materialized-spec `mono_sig_ok` signature
  classification) -- the analysis foundation.
- **G3a** -- self-contained island monomorphs emit `<clone>__cps` + wrapper,
  callers unchanged, no taint/routing change. Fixture `cps-backend-generic-island`.
- **G3b** -- cross-function chains. The "ordering knot" turned out to be a
  non-issue (`emit_abi_scan_program` completes the spec set before `ensure_S`),
  so the taint fixpoint ranges over monomorphs via a `mono_template` flag (a
  generic whose every monomorph is admissible does not taint), non-island
  monomorphs emit DK, and cps->cps callers route to `<clone>__cps`. Fixture
  `cps-backend-generic-cross-fn`. Full suite 2039/0.

Remaining tails: the G2 body-resolution note (user-defstruct-result bare `TY_ADT`
still under-reports in the probe, a narrow analysis gap) and the G3b ambiguous-
call routing residual (loud link error, no fixture hits it). Both are documented
in their entries; neither blocks the landed emit.

**G3a is landed**: a colored-generic monomorph that is a self-contained island
(every effect handled within, no colored callee) now CPS-emits (DK) as
`<clone>__cps` + a `<clone>` wrapper, callers unchanged, no whole-program taint or
routing change. The carrier-crossing that blocked a first attempt is fixed (set
`emit_byvalue_carrier_abi` on concrete by-value ADT-app spec params in the
island-emit path, mirroring the fiber `emit_params`). Fixture
`cps-backend-generic-island`; suite 2038/0. See the G3a entry for details.

**G3b** (cross-function DK chains) was investigated this pass and its full,
five-part implementation spec is in the G3b entry -- but it is NOT landed and,
unlike G1/G2/G3a, has **no safe incremental slice**: taint surgery, per-call-site
routing, a CPS-IR change, and a pre-emit discovery pass are all interlocking
(the taint change is a silent miscompile without routing+emit; routing is
untestable without a DK caller that the taint gates). It is a dedicated
correctness-critical pass with real regression risk on the 2038 fixtures. The
current behavior is sound (these monomorphs run on fiber via effect-taint). Since
G3a already captured the safe self-contained subset, the recommendation is to
schedule G3b as its own focused effort rather than fold it into an incremental
pass.

G3 was investigated in depth this pass with two structural findings (see the G3
entry): (A) the CPS emit path is **already spec-aware** (type spellings resolve
via `emit_type_c_name` -> `emit_resolve_type(current_abi_specialization)`), so the
feared "thread substitution through the emitter" work is already done -- a
monomorph reuses the generic CTerm and the spellings resolve at emit; (B) the
real blocker is the **safety gate**, not the emit plumbing.

The cleanest G3 architecture that fell out of this: rather than build a new
escaping-effect analysis, **reuse the existing (battle-tested) `ensure_S` taint
fixpoint as the safety gate.** Make a colored generic *template* a taint
participant whose candidacy is decided by the fixpoint (classify its body
permissively for tyvar types; verify each concrete monomorph's signature via
`mono_sig_ok` at emit and fall back per-monomorph if a specific instantiation is
non-slot). If the template co-classifies (no fiber-only peer shares its effects),
every monomorph is safe -- self-contained or cross-function -- because the
fixpoint already guaranteed peers agree. Then emit each monomorph as
`<clone>__cps` + wrapper (skipping the template body, which is never emitted).

The risk is concentrated and real: making a template a taint participant changes
whole-program classification (e.g. `run` in the cross-machine case stops being
evicted), which flips emit output for those functions and must be staged with the
fixture suite watched at each micro-step (forward-decl emission must exclude the
never-defined template `<generic>__cps`; per-monomorph signature fallback must be
correct). This is a materially larger, higher-risk change than the prior N6
slices, and the current behavior is already sound (effect-taint keeps generic
monomorphs on fiber without splitting any DK chain).

Next-step options, in the spirit of the one-track-to-v1 priority:

- **Proceed to G2** -- factor a shared monomorph-discovery pass that enumerates
  specs before `ensure_S`'s taint fixpoint, and upgrade the probe's resolver to
  the full monomorph-ADT materialization (closing the user-defstruct
  false-negative). Still landable dump-only before any emit change.
- **Defer G2-G4** and keep picking off smaller residuals (the remaining small
  items are effectful callbacks and the delimited-control tail), returning to
  this once those are exhausted.

## Depends on / reuses

- `EmitAbiSpecialization` + `emit_abi_intern_spec` (emit_module.c) -- the spec
  registry to enumerate/reuse.
- `ensure_S` taint fixpoint (emit_cps_ir.c) -- must range over monomorphs.
- `emit_cps_ir_try_fn` (emit_cps_ir.c) -- the per-fn (now per-monomorph) hook.
- Parent: [cps-backend-n6-fallback-removal-plan.md](cps-backend-n6-fallback-removal-plan.md).
