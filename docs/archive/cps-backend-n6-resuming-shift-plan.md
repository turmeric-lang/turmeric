---
title: Resumable delimited control -- one substrate (k-reset / k-shift)
category: Planning
status: in progress -- slices 1-4 LANDED (k-reset/k-shift on the cloneable substrate; (k v) sugar via `k : cont`; single-shot via `^linear k : cont`, compile-time TUR-E0101; typed `Cont<BodyT,ResetT>` via `(cont BodyT ResetT)`, resume-value checked). Consolidation IN PROGRESS: unification LANDED -- ONE shift/reset pair now serves abortive + resumable + single-shot + typed + cross-function-abort, dispatched by the receiver's convention (cont-typed -> continuation-passing; plain -> abortive), with `reset` auto-promoting to the reified delimiter only when a resuming shift binds. The interim k-shift/k-reset scaffolding is RETIRED (item d), so the plan adds no net new keywords. Open: cross-function RESUME (a __Shift-effect desugar, its own change), folding cloneable-*/serial- into shift/reset with a capability annotation, optional lighter single-shot runtime.
description: Split out of cps-backend-n6-fallback-removal-followups-plan.md (Task 1). Rather than reinterpret the abortive `shift` (breaking ~36 fixtures + the interpreter), add a NEW resumable delimited-control surface (`k-reset` / `k-shift`) that lowers to the SAME substrate the cloneable/serial variants already use -- the DK machine in compiled code, the reified-context TuriCont in the interpreter. `cloneable-*` and `serial-*` are capability-specializations that fold into this primitive; abortive `shift`/`shift0` is a distinct dynamic-abort lowering that the unified SURFACE routes to (not deletes) -- see Consolidation.
---

# Resumable delimited control -- one substrate

## Direction (decided)

The abortive `shift` is not made resumable in place -- that would reinterpret
every existing `(shift (fn [v] ...) body)` site (~36 fixtures + the turi
interpreter tests) and change core semantics. Instead we add a **new resumable
surface** and, over time, unify the surfaces.

Guiding realization (the whole reason this is one plan, not four): resumable
delimited control is **already one substrate**. In compiled code the DK machine
(`dk_shift` / `dk_invoke` / `dk_copy_range`, `src/compiler/emit_dk_runtime.c`)
backs abortive shift, cloneable, and serial alike. But note the two *lowerings*
are genuinely different in reach: abortive is a **dynamic abort** (works
cross-function, any context, no reification), while cloneable/serial/k-shift
**reify the context syntactically** to make `k` resumable (lexically scoped,
subset-restricted). So the continuation *capability* (single-shot / clone /
marshal) is one axis, but abort-vs-reify is a second, real axis -- unifying the
surface keeps **both** lowerings underneath (see Consolidation). The new surface
reuses the existing machinery; it does not add a fifth machine.

## The new surface -- `k-reset` / `k-shift`

`(k-reset BODY)` delimits; `(k-shift receiver body)` captures the delimited
continuation `k` and hands it to `receiver`, a `(fn [k] ...)` that resumes `k`.
This is the standard shift/reset -- the receiver gets the continuation, NOT the
body value (which is what distinguishes it from abortive `shift`). Spelled with a
`k-` prefix so it coexists with the abortive `shift`/`reset` until those retire.

### As it works TODAY (slice 1, cloneable-backed)

Slice 1 aliases `k-reset`/`k-shift` onto the cloneable pipeline verbatim, so the
current surface is exactly cloneable's: the continuation is a **multi-shot**
cloneable handle, resumed with the explicit `tur_cloneable_cont_resume` /
`tur_cloneable_cont_clone` primitives -- **not** `(k v)` application sugar (that
sugar does not exist yet; `(k 1)` errors with "not a function or continuation").

```turmeric
(defn step [k] : int                                   ; k : cloneable-cont handle
  (+ (tur_cloneable_cont_resume (tur_cloneable_cont_clone k) 1)  ; resume once
     (tur_cloneable_cont_resume k 2)))                 ; resume again (multi-shot)

(k-reset (+ 10 (k-shift step 0)))   ; context (+ 10 []): (10+1) + (10+2) = 23
```

Inherited cloneable limitations apply today: only the native `build_cloneable`
context subset is supported (arithmetic binops, calls, pure lets, one `if`
branch point); a context outside it is rejected with `TUR-E0710`, not
miscompiled. The 2nd operand (`0` above) is the cloneable-shift seed/hole slot.

### The intended surface -- mostly already composes from existing features

The ergonomic and single-shot surfaces do **not** need new machinery: they fall
out of the existing continuation-type spellings (`cont` / `serial-cont` /
`escape-cont`), the `(k v)` application sugar (CC4, `elab_call.c:3512`), and
`^linear` one-shot enforcement.

- **`(k v)` resume sugar -- WORKS.** Type the receiver param `k : cont` and
  `(k v)` resumes it directly: `(defn step [k : cont] : int (+ (k 1) (k 2)))`.
  No need for `tur_cloneable_cont_resume`. (It only failed earlier because an
  *untyped* `k` is not a `cont`.)
- **Single-shot `k-shift` -- LANDED (slice 3).** A single-shot continuation is a
  `^linear` one: `(defn f [^linear k : cont] : int (k 5))`. Resuming twice is a
  **compile-time** error (`TUR-E0101`, use-after-consume) -- strictly stronger
  than the runtime error originally envisioned, and enforced on all three paths
  because the program never compiles. Reuses the shared substrate unchanged.
  Fixtures: `k-shift-single-shot` (resume once, `direct == cps == turi`) and
  `errors/k-shift-single-shot-double-resume` (double `(k v)` -> `TUR-E0101`).
- **Lighter single-shot RUNTIME (optional, not required).** A single-shot
  `k-shift` still resumes over the cloneable-capable runtime today; a genuinely
  lighter representation (plain `dk_shift`/`dk_invoke`, no `dk_copy_range` clone
  glue) is a pure optimization, tracked separately -- it is not needed for
  single-shot *semantics*, which linear typing already guarantees.
- **Typed continuation (slice 4) -- LANDED.** `(cont BodyT ResetT)` pins the
  resume-value type; `(k v)` checks `v : BodyT` (`TUR-E0001` on mismatch).
  `k : cont` / `(cont R)` stay unchecked for backward compatibility.

## Work plan (incremental, each slice testable)

**Slice 1 -- LANDED.** `k-reset` / `k-shift` recognized as canonical resumable
delimited control, aliased onto the cloneable pipeline (the proven shared
substrate). Because the alias reuses the whole cloneable path, the entire
lowering came in one move -- **no new pipeline code**:

- Symbols `sym_k_reset` / `sym_k_shift` (`elab_internal.h`, `elab_core.c`);
  dispatch to `elab_cloneable_reset` / `elab_cloneable_shift` (`elab_call.c`).
- Type-checking, direct emission on `dk_shift`/`dk_invoke`, and the CT-IR
  lowering are all inherited from the cloneable pipeline unchanged.
- Fixture `k-shift-resumable-basic`: `k-shift` receiver resumes `k` (twice,
  cloneable); `direct == cps == turi == 23 / 6`.

The trade-off this bought: the surface is exactly cloneable's today (multi-shot,
explicit resume primitive, `TUR-E0710` context subset). Slices 2-4 make it
ergonomic and add the single-shot/typed variants.

**Slice 2 -- ALREADY AVAILABLE -- `(k v)` resume sugar.** Works today by typing
the receiver param `k : cont`; the CC4 sugar (`elab_call.c:3512`) resumes the
handle. No new code was needed -- the earlier `(k 1)` failure was only because
an untyped `k` is not a `cont`. Exercised by both slice-3 fixtures.

**Slice 3 -- LANDED -- single-shot `k-shift`.** A `^linear k : cont` receiver is
single-shot: `(k v)` resumes once, a second `(k v)` is a compile-time
`TUR-E0101` use-after-consume. This is stronger than a runtime error (the
program never compiles, so all three paths agree) and reuses the shared
substrate with no new lowering. Fixtures: `k-shift-single-shot` (resume once,
`direct == cps == turi == 15 / 12`); `errors/k-shift-single-shot-double-resume`
(`TUR-E0101`). A lighter single-shot *runtime* (no `dk_copy_range` clone glue)
remains an optional optimization, not a semantics blocker.

**Slice 4 -- LANDED -- fully typed continuation `Cont<BodyT,ResetT>`.** The
two-arg annotation `(cont BodyT ResetT)` now pins the resume-value type: the
`cont` type carries an `arg` (BodyT) alongside `returns` (ResetT), and the
`(k v)` sugar checks `v : BodyT`, rejecting a mismatch with `TUR-E0001`. The
one-arg `(cont R)` / bare `cont` spellings keep `arg = TY_UNKNOWN` (unchecked),
so every existing signature is unaffected.

- `types.h`: `cont.arg` field + `type_cont_arg_flavored`.
- `elab_fns.c`: parse `(cont BodyT ResetT)` (three-form list).
- `elab_call.c`: the `(k v)` resume-value type check.
- Fixtures: `k-shift-typed-cont` (`(cont int int)`, resume with int,
  `direct == cps == turi == 15`); `errors/k-shift-typed-cont-mismatch`
  (resume with cstr -> `TUR-E0001`).

## Consolidation (the retirement path -- OPEN, larger)

- **Abortive `shift` -- NOT a simple desugar (investigated 2026-07; see
  [abortive-shift-retirement-blocked.md](../../reported/abortive-shift-retirement-blocked.md)).**
  The naive "abortive shift == k-shift whose receiver ignores `k`" desugar
  regresses well over a third of the abortive corpus: abortive aborts
  *dynamically* (works cross-function and under arbitrary contexts), whereas
  k-shift/cloneable reifies the context *syntactically* -- so a cross-function
  abortive shift becomes `TUR-E0016` and a non-subset context becomes
  `TUR-E0710` (both proven empirically; ~13 of ~36 fixtures are cross-function
  alone). The abortive lowering is therefore **load-bearing, not redundant**;
  deleting `eval_abortive_shift` / `emit_effects_shift` is wrong.

  The correct end state is **surface unification with dual lowering**: one
  `shift`/`reset` keyword pair, routed by whether the receiver uses its
  continuation -- ignore-`k` -> the abortive DK fast-path (dynamic,
  cross-function); resume-`k` -> the reified-context path (today's cloneable,
  lexically scoped) or the CT-IR DK-subk threading for cross-function resume.
  Both runtimes stay; only the surface collapses. This is a real design effort,
  not a fixture migration.

  **Unification slice 1 -- LANDED.** `k-shift` now IS this dual-lowering surface.
  When its receiver provably ignores `k` (a see-through lambda/closure whose body
  never references the continuation param), `elab_cloneable_shift` routes it to an
  abortive `EX_SHIFT` (dynamic-abort path) instead of the reified path -- so an
  ignore-`k` `k-shift` works **cross-function** and under **arbitrary contexts**,
  no `TUR-E0016` / `TUR-E0710`. Enabled by two proven facts: abortive `shift`
  aborts to a `k-reset` prompt (the DK prompts are compatible), including
  cross-function. The routing is **purely additive** -- gated on `k-shift` (not
  `cloneable-shift`) and on a receiver we can prove ignores `k`; resuming k-shifts
  and all `cloneable-*` are untouched. Impl: `receiver_ignores_continuation` +
  the desugar in `elab_effects.c` (`(k-shift R body)` with ignore-`k` `R` ->
  `(shift R <null-cont>)`, reusing the working abortive lowering -- no new
  lowering sites). Fixture `k-shift-abort-crossfn` (cross-function + context-
  discard, `direct == cps == turi == 105 / 7`).

  **Unification slice 2 (item A) -- LANDED.** The ignore-`k` analysis now also
  sees through **named-fn receivers** via `Binding.source_fn_def` (the FnDef a
  top-level `defn` binding defines, set during elaboration). So a named receiver
  whose body never references its continuation param routes to the abort path
  too -- cross-function, any context -- which is where most real abortive code
  lives. Still conservative on a forward-referenced receiver (source_fn_def not
  yet set -> reified path). The continuation param index is computed precisely
  (lambda/named at 0, closure env-prepended at 1) and only single-continuation
  receivers route. Fixture `k-shift-abort-crossfn` extended with a named case.

  **Unification slice 3 (item B) -- LANDED.** One `shift`/`reset` keyword pair
  now serves every mode, dispatched by the receiver's convention:
  - `shift` with a `cont`-typed receiver -> the continuation-passing path
    (resume-k reified, or ignore-k dynamic abort); a plain-value receiver keeps
    the abortive convention. `elab_shift` dispatches on the receiver's parameter
    type (`shift_fn_domain_codomain` domain == `TY_CONT`).
  - `reset` promotes itself from the abortive `EX_RESET` to the reified
    `EX_CLONEABLE_RESET` **only when a resuming shift binds to it** (tracked by a
    per-depth flag set in the reified shift path). A reset with only abortive
    shifts / `shift0` stays `EX_RESET` -- which they need: a blanket
    `reset -> cloneable-reset` alias was measured to break 8 fixtures (shift0,
    nested resets, continuation-substrate); the conditional promotion breaks none.
  So plain `shift`/`reset` cover abortive, resumable, single-shot (`^linear
  cont`), and cross-function; `k-shift`/`k-reset`/`cloneable-*` are now redundant
  at the surface. Fixture `shift-unified-keyword`. Full suite 2115, 0 failed.

  **Item (c) -- cross-function resume -- investigated, scoped as its own change
  (see [cross-function-resume-design.md](../../reported/cross-function-resume-design.md)).**
  A resuming shift with the reset in a caller is fundamentally not expressible on
  the reified (lexically-scoped) path -- and it is the deep first-class-
  continuations work. Key finding: `perform`/`handle`/`resume` ALREADY do
  cross-function resumable continuations on both paths (`dk_invoke` compiled,
  fibers in the interpreter; verified `direct == turi == 1050`). So the tractable
  design is a **shift/reset -> synthetic `__Shift` effect desugar**, not new
  DK-subk plumbing. The blocker is a continuation-representation mismatch (a shift
  receiver's `cont` / `(k v)` vs an effect handler's `resume k`), whose alignment
  (a `CONT_EFFECT` flavor, or a `(k v)`->`resume` rewrite) is a real multi-part
  change spanning the type system + elaboration -- a plan of its own, not an
  incremental slice.

  **Foundation landed.** The resume-representation mismatch (the first blocker) is
  resolved: `(k v)` now works on an effect handler continuation (routes to
  `EX_RESUME` via a shared `elab_make_resume`), so a receiver `(fn [k] (k v))`
  resumes a handler continuation uniformly -- verified cross-function (`perform`
  in a callee, `handle` + `(k 5)` in a caller, `direct == turi == 1050`; fixture
  `handler-cont-kv-sugar`). No `CONT_EFFECT` type flavor was needed -- the
  `is_continuation` flag is the hook and `resume` already accepts a continuation
  *value*.

  **Blocker 3 (effect fn payloads) -- RESOLVED.** Effects now carry a callable fn
  payload, capturing or not. An fn payload is a **boxed closure** (one-word
  `void *` to a heap `{thunk, env}` box) so a captured env fits the one-word
  effect slot: `defeffect` preserves + marks `boxed` a `TY_FN` param, and
  `elab_perform` boxes a non-capturing fn arg (`EX_FN_TO_FAT`) for a uniform
  representation. Verified `direct == cps == turi` for capturing on both the
  receiver and handler sides (fixtures `effect-fn-payload`,
  `effect-fn-payload-capturing`). This is the `__Shift` receiver shape, so the
  effect-desugar path is unblocked on the payload.

  So of the cross-function-resume prerequisites, blockers 1 (resume mismatch) and
  3 (callable + capturing fn payloads) are DONE, and the reset->handle +
  shift->perform **mechanism is verified end to end** -- a hand-written `__Shift`
  desugar runs `direct == turi == 15`, and cross-function resume with a capturing
  receiver is pinned by fixture `cross-function-resume-via-effect` (= 11). So
  cross-function resumable continuations WORK today via the effect surface, with
  the ergonomic `(k v)` resume.

  What remains is the **automatic desugar** of the `shift`/`reset` surface onto
  this machinery (blocker 2) -- a substantial multi-part change, not a bounded
  slice: (1) synthesize the `__Shift` effect; (2) `shift`->`perform` at the
  `TUR-E0016` site; (3) a whole-program `reset`->handle-wrapping pass (gated so
  the common case is byte-for-byte unchanged); (4) the receiver-convention crux
  (the user's `(fn [k : cont] (k v))` uses *cloneable* resume, cross-function
  needs *effect* resume -- a `CONT_EFFECT` flavor / Form-rewrite / runtime bridge
  / runtime-polymorphic dispatch). Details + the four bridge options in the design
  doc. Also landed earlier: a tailored `TUR-E0016` message (fixture
  `errors/shift-crossfn-resume`). **The auto-desugar is scoped as its own story:**
  [cps-backend-n6-crossfn-resume-desugar-plan.md](cps-backend-n6-crossfn-resume-desugar-plan.md)
  (slices: `CONT_EFFECT` flavor -> `__Shift` synthesis + `shift`->`perform` ->
  gated `reset`->handle-wrapping pass -> end-to-end fixture).

  **Item (d) -- retire the redundant spellings -- PARTIALLY LANDED.** `k-shift` /
  `k-reset` (the interim canonical names this plan introduced) are **removed**:
  plain `shift`/`reset` now cover them (a `cont`-typed receiver routes `shift` to
  the continuation-passing path). The session therefore adds **no net new
  keywords** -- `shift`/`reset` (which already existed) became the unified
  surface, and the scaffolding names are gone. Symbols/dispatch/gates deleted
  (`elab_internal.h`, `elab_core.c`, `elab_call.c`, `elab_effects.c`); the four
  `k-shift-*` fixtures migrated to `shift-*`.

  Still standing (a separate, larger capability-folding effort -- NOT pure
  aliases): `cloneable-*` and `serial-*`. `cloneable-shift` is reachable via
  `shift` with a `cont` receiver, but its untyped-receiver + explicit-primitive
  (`tur_cloneable_cont_resume`) style does not migrate cleanly (a `cont`-typed
  handle is rejected by the `:int` primitives). `serial-*` is a genuinely
  distinct capability (marshalable): `shift` with a `serial-cont` receiver
  currently yields cloneable, not serial, so folding it needs `shift` to preserve
  the continuation's flavor. Both are explicit-capability spellings for now.

  Remaining: (c) the `__Shift`-effect desugar (cross-function resume); the
  cloneable/serial capability-folding above.
- **cloneable-shift / serial-shift** become `k-shift` with a *continuation
  capability* (cloneable = multi-shot clone; serial = marshalable). The capture
  machinery is already shared; only the capability annotation differs. Collapse
  the three receiver-lowering paths into one. Done when `cloneable-*`/`serial-*`
  are thin capability annotations over the k-shift path, not separate pipelines.
  (This one is a genuine unification -- all three already use the reified-context
  path -- unlike the abortive case above.)

The end state is a single delimited-control *surface* with a capability knob on
the continuation and two lowerings underneath (dynamic-abort vs reified-resume) --
the "one substrate" this plan is named for. Each step is independently landable
and suite-gated.

## Where this came from

Task 1 of
[cps-backend-n6-fallback-removal-followups-plan.md](../../archive/cps-backend-n6-fallback-removal-followups-plan.md)
(archived). Analysis of why the in-place reinterpretation is a breaking language
change:
[cps-backend-n6-fallback-followups-blocked.md](../../reported/cps-backend-n6-fallback-followups-blocked.md).
Prior art on the abortive limitation:
[compiled-first-class-continuations-plan.md](../../archive/compiled-first-class-continuations-plan.md).

## Out of scope

- Deleting the general fallback -- its own plan
  ([cps-backend-n6-fallback-deletion-plan.md](cps-backend-n6-fallback-deletion-plan.md)).
- Changing abortive `shift` semantics in place (explicitly rejected above).
