---
title: CPS backend -- ref<T> scope-exit auto-drop in colored functions (O1-b)
category: Planning
status: P1 landed (generalized to all owning locals -- rc + ref); P2 single-shot-crossing slice landed; only P2 abortive + P3 multi-shot crossing still fall back. Substrate (cps-tramp-resume, owning-cloneable-capture) graduated to always-on 2026-07-19.
description: A `ref<T>` (also `weak`/`lref`) local gets a `(defer (drop! r))` injected at let scope exit (elab_forms.c). In an uncolored function the direct path fires it through the tur_frame LIFO defer stack. In a colored (CPS-emitted) function there is no equivalent: `EX_DEFER` has no CT-IR lowering case, so the whole function falls back, and even if it were lowered the injected drop lands textually after the control op, which `owning_dropped_before_control` rejects. This plan defines and implements ref scope-exit drop under CPS: P1 hoists the drop to last-use for a ref that does not cross a control op (self-contained, no runtime); P2/P3 (a ref live across an abortive or resumable control op) ride the DK-teardown / clone-drop discipline from the env-capture plan. Sound on the fallback today -- missed coverage, not a correctness gap.
---

# CPS backend -- ref<T> scope-exit auto-drop (O1-b)

> **Progress note (2026-07-22) -- re-verified; plan was under-reporting. Two
> things have moved since the 2026-07-19 note below.**
>
> 1. **P1 landed AND was generalized beyond `ref<T>`** to all owning locals
>    (rc + ref). The hoist recognizer is `autodrop_defer_owning`
>    (`src/passes/cps_ir.c:2239`, via `autodrop_owning_kind` / `autodrop_root_local`),
>    NOT the `autodrop_defer_ref` named in the "P1 as landed" section below --
>    that symbol does not exist; read every `autodrop_defer_ref` there as
>    `autodrop_defer_owning`. Emitter: `plan_autodrop`
>    (`cps_ir.c:2531`), `cps_emit_hoisted_drops` (`:2601`). Commit `de6a2e6b0`.
>    Fixtures: `cps-backend-ref-noncrossing-drop` and the generalized
>    `cps-backend-owning-autodrop-noncrossing`.
>
> 2. **The P2/P3 substrate the note below calls "not built" has GRADUATED to
>    always-on** (2026-07-19): `cps-tramp-resume` (`g_opt_cps_tramp_resume`
>    defaults true, `src/runtime/experiments.c`) and owning-cloneable-capture
>    (commit `bdb3385a2`). Riding that, **the single-shot slice of P2 landed**:
>    `plan_autodrop` has a crossing branch (`cps_ir.c:2578-2592`) gated by
>    `expr_has_unsafe_control` (`:2349`, "P2 gate"), and `ref_dropped_before_control`
>    now GRANTS the single-shot-continuation drop pass at PERFORM/HANDLE/RESET/AWAIT
>    (`emit_cps_ir.c:2016-2026`) -- which directly inverts the "a ref gets NO
>    single-shot pass" paragraph in the "P1 as landed" section below (that
>    paragraph is now false). Commit `c54789ddf`; fixture
>    `cps-backend-owning-autodrop-crossing-singleshot` (`b9590f023`).
>
> **Still open (narrowed):** only a *genuinely abortive* crossing (`shift` /
> discontinue that discards the continuation) and a *multi-shot* resumable
> crossing still fall back (`expr_has_unsafe_control` returns false ->
> fallback, `cps_ir.c:2355-2360`). P3's O3 substrate has graduated, so P3 is now
> *buildable*, not *blocked* -- it just is not wired to a ref-multishot auto-drop
> path yet. Line-number citations in the older sections below have drifted (the
> gate trio is now `emit_cps_ir.c:1983/2003/2039`, not `:1890-1922`).
>
> **Progress note (2026-07-19).** Verified against the tree. **P1 is landed.**
> The hoist (`plan_autodrop` + the widened `autodrop_defer_owning` recognizer in
> `src/passes/cps_ir.c`) and the soundness gate (`ref_dropped_before_control` /
> `is_ref_drop_of` in `emit_cps_ir.c:1890-1922`, consulted by `letraw_ok`) are
> both present. Fixture `tests/fixtures/cps-backend-ref-noncrossing-drop` exists
> (`input.tur` + `expected.stdout`, no `requires.no-leak-check`). **P2 (abortive
> crossing) and P3 (resumable crossing) remain open**, both gated on the E3
> DK-teardown / env-capture clone-drop substrate, which is not built (see Track B's
> E3 section of the env-capture plan,
> `cps-backend-multishot-continuations-owning-capture-plan.md`) -- so a crossing
> ref continues to fall back correctly. This plan stays
> **OPEN on P2/P3 only**, per the original caveat: they wait for a real
> crossing-ref case plus the teardown.

## Why this document exists

O1-b in
[cps-backend-owning-pointers-followups-plan.md](../archive/cps-backend-owning-pointers-followups-plan.md)
("`ref<T>` scope-exit auto-drop / `EX_DEFER`") was deferred as "documented
low-value -- keep on fallback," with the caveat *"Non-goal unless a real
`ref`/`weak`/`lref`-in-colored-function case shows up that would actually
CPS-emit -- revisit then."* This document is that revisit: it states exactly what
"actually CPS-emit" would require, splits the tractable slice from the slice that
depends on other work, and gives the tractable slice a concrete landing.

**Nothing here is a correctness gap today.** A `ref<T>` in a colored function
compiles and runs correctly by falling back to the general whole-function
lowering. This is a *coverage* item: extend the CT-IR backend so the tractable
subset CPS-emits instead of falling back, and pin the rest to the env-capture
work so it is not lost.

## The mechanism today (verified)

A `ref<T>` local (a `TY_REF` binding not sourced from `EX_REF_FROM_RC`) owns its
referent, so elaboration injects a single ownership discharge at scope exit:
`(defer (drop! r))`, appended to the enclosing `let` body
(`src/compiler/elab_forms.c:877-893` detects the ref bindings;
the injection loop builds `EX_DEFER` around a `drop!` call, `~:1024-1051`). The
same discharge applies to the other linear owning locals that route through
`defer`.

- **Uncolored function (direct path):** the `do`-block lowering sets up a
  `tur_frame`, registers each defer thunk with `tur_frame_push_defer`, and fires
  them LIFO at scope exit (`tur_frame_fire_lifo`) or on an early return / throw
  (`tur_frame_fire_chain`) -- `src/compiler/emit_expr.c:1946-2058`, runtime in
  `src/runtime/runtime.h`. This correctly handles sequential exit, early return,
  and unwinding throw.

- **Colored function (CPS path):** two independent blocks:
  1. `EX_DEFER` has **no case** in the CT-IR lowering switch
     (`src/passes/cps_ir.c`); it appears only in auxiliary walks
     (`body_calls_binding`, `src/passes/cps_ir.c:2032`; the free-var REC at
     `src/compiler/emit_cps_ir.c:1774`). An unmodeled node returns `NULL`, so a
     colored function still carrying a `ref` auto-drop at lowering time falls
     back -- it never reaches CPS emission.
  2. Even if it were lowered, the injected `(defer (drop! r))` sits at scope exit
     -- textually *after* any control op inside the scope. On the CPS path a drop
     after the control op lands in the reified continuation, so
     `owning_dropped_before_control` (`emit_cps_ir.c:1043`) rejects it (an
     abortive `shift` discards that continuation -> the drop is lost; a resumable
     one runs it per-resume -> double free). That guard is what keeps the naive
     translation from ever being unsound; today it simply forces fallback.

So O1-b is not one hole but a spectrum keyed on the ref's live range relative to
the control op.

## Case analysis

Let `r` be the `ref<T>` local and `C` the control op (reset / handle / perform /
shift) inside `r`'s scope.

1. **`r` does not cross `C`** -- `r` is created and last-used entirely before `C`
   (or entirely after, in a branch that reaches no `C`), and its value is not
   captured by the continuation. The scope-exit drop is *semantically* a
   drop-at-last-use hoisted to the wrong place. If we drop `r` at its true last
   use -- before `C` -- `owning_dropped_before_control` is satisfied, no frame is
   needed, and the function CPS-emits. **Tractable, self-contained (P1).**

2. **`r` crosses `C`, `C` is abortive** (continuation discarded). The drop must
   fire when the delimited continuation is abandoned -- a frame-unwind exactly
   like the direct path's `tur_frame_fire_chain`, but driven by DK teardown
   rather than a C-stack unwind. Requires a DK-node teardown that fires
   registered drops. **Depends on the DK-teardown substrate (P2).**

3. **`r` crosses `C`, `C` is resumable / multi-shot.** `r` is captured into the
   continuation env; its drop must fire once per continuation *lifetime*, not
   once per resume. This is exactly the O3 owning-capture hazard -- one shallow
   env copy, N body drops. It rides the per-capture clone/drop discipline in
   [cps-backend-multishot-continuations-owning-capture-plan.md](../archive/history/cps-backend-multishot-continuations-owning-capture-plan.md).
   **Is O3-shaped (P3).**

The owning-pointers follow-ups already observed that a real `ref`-in-colored
function "almost always falls back regardless" -- that assessment is about cases
2 and 3, which need substrate that does not exist yet. Case 1 is the slice that
does not, and P1 converts exactly that slice from fallback to CPS-emit.

## Design

### P1 -- drop-hoisting for a non-crossing ref (recommended first landing)

Add a CT-IR lowering for the injected `(defer (drop! r))` **restricted to the
non-crossing case**, implemented as a hoist rather than a deferred thunk:

- Recognize the elaborator-injected shape: an `EX_DEFER` whose body is a `drop!`
  of a single `ref`/`weak`/`lref` local (the auto-drop shape, distinguishable
  from a user `defer` of arbitrary effects -- gate P1 to the auto-drop shape
  only; a general user `defer` in a colored function keeps falling back).
- Compute `r`'s last use within the CPS term and whether it precedes every
  control op on its path (reuse the control-op seed set already used by
  `operand_uses_control` / `cps_directly_uses_control`, and the liveness the
  `owning_dropped_before_control` guard already reasons about).
- If `r` is dead before every reachable control op and is not captured by any
  continuation, **emit the drop at that last-use point** (a straight-line
  `CT_LETRAW` drop, the same node O1-a already delegates for explicit `rc`
  drops), and *drop the `EX_DEFER`* from the lowered term. The existing
  `owning_dropped_before_control` / `letraw_ok` guard then passes on the
  hoisted node.
- If `r` crosses a control op or is captured, **keep falling back** (do not
  emit) -- P2/P3 own those. Never emit a drop that the guard would have to
  reject.

P1 needs no runtime and no new node kind; it is a targeted early-drop rewrite for
the auto-drop `EX_DEFER` shape plus the CT-IR lowering case that consumes it.

Fixture `tests/fixtures/cps-backend-ref-noncrossing-drop`: a colored function
creates a `ref<T>`, uses and finishes with it before a `shift`, delivers a
scalar; `direct == cps`, LeakSanitizer-clean (the hoisted drop runs exactly
once).

#### P1 as landed

Implemented across two files:

- **The hoist (`src/passes/cps_ir.c`).** `plan_autodrop` recognises a `do`
  ending in one or more `(defer (drop! r))` auto-drop shapes (`autodrop_defer_ref`
  gates to the `drop!` free-shape builtin on a single `ref<T>` var, so a general
  user `defer` never engages), finds the first control barrier among the real
  items (`item_has_control`, mirroring `cps_directly_uses_control`'s seed set),
  and verifies no ref is referenced at/after it (`expr_refs_binding`, conservative:
  an un-modelled node or an unwalked closure capture assumes a use and bails). The
  `EX_DO` arms of `cps_tail` / `cps_bind` then emit the drops via `build_letraw`
  (`cps_emit_hoisted_drops`) straight-line *before* the first barrier, or at scope
  exit (after the value, before delivery) when the ref's `do` has no barrier. The
  ref constructor itself (`EX_REF`) is added to `safe_to_delegate` so the whole
  function can leave the fallback path.

- **The soundness gate (`src/compiler/emit_cps_ir.c`).** A delegated `EX_REF`
  alloc is admitted by `letraw_ok` only when `ref_dropped_before_control` finds its
  `drop!` (`is_ref_drop_of`) on a straight-line path before *any* control op.
  Unlike `owning_dropped_before_control` for rc handles, a ref gets **no**
  "captured into a single-shot continuation, dropped there" pass at
  reset/handle/perform (that DK teardown is the unbuilt P2/P3 substrate), so a ref
  crossing any control op -- abortive or delimited -- fails the gate and falls
  back. This is the backstop: even a crossing ref whose auto-drop defer is not
  hoisted (its `EX_DEFER` stays unlowered -> `CT_UNSUPPORTED`) is *also* rejected
  here, so no ref reaches emission without a hoisted drop preceding every control
  op. `plan_autodrop` reads the source `Expr` and builds a fresh CT term without
  mutating it, so a rejected lowering falls back to the direct emitter on the
  intact `Expr` (defer included).

### P2 -- abortive-unwind drop firing (DK teardown)

For a ref live across an abortive control op, register `r`'s drop so DK teardown
fires it when the continuation is abandoned -- the CPS analogue of
`tur_frame_fire_chain`. This needs the DK nodes to carry a teardown that runs
registered drops, which is the same missing substrate the env-capture plan builds
for owning captures (the CT-IR reset/handle DK nodes are leaked today; giving
owning-carrying frames a teardown is E3 there). **P2 lands on top of that DK
teardown, or is co-designed with it.** Until then, case 2 falls back (sound).

### P3 -- resumable-crossing ref (rides O3)

A ref captured by a resumable continuation is an owning capture. Route it through
the env-capture clone/drop discipline (E1-E4 of the env-capture plan): the ref's
drop becomes the per-capture drop callback, fired once per continuation lifetime.
No separate mechanism -- P3 is "the captured value happens to be a `ref`." Land
after the env-capture plan's E-series covers owning captures.

## Phasing / recommendation

Land **P1** as the standalone O1-b win: it is self-contained, needs no runtime,
and closes the one slice the follow-ups called out as "could translate." **P2**
and **P3** are explicitly gated on the DK-teardown / env-capture substrate and
should be co-scheduled with
[cps-backend-multishot-continuations-owning-capture-plan.md](../archive/history/cps-backend-multishot-continuations-owning-capture-plan.md)
(P2 with its E3 DK teardown, P3 with its E1-E4 owning-capture clone/drop) rather
than reinvented here. If P1's measured payoff is as small as the follow-ups
predicted, P1 is still worth landing to shrink the fallback surface N6.5 must
account for; P2/P3 wait for a real crossing-ref case, per the original caveat.

## Interaction with other plans

- **O1-b** in the owning-pointers follow-ups is *this*. Its state line now points
  here.
- **Env-capture plan** owns the substrate P2 (DK teardown, E3) and P3 (owning
  capture clone/drop, E1-E4) depend on. P2/P3 must not build a parallel teardown
  path.
- **N6.5** (fallback deletion,
  [cps-backend-n6-fallback-removal-followups-plan.md](../archive/cps-backend-n6-fallback-removal-followups-plan.md)):
  a colored function with a `ref` auto-drop is a residual fallback case. P1
  removes the non-crossing subset; the crossing subset stays fallback (or a named
  carve-out) until P2/P3 land. List that explicitly when N6.5 audits residuals.

## Depends on / reuses

- The auto-drop injection: `EX_DEFER` around `drop!` for `TY_REF` locals
  (`src/compiler/elab_forms.c:877-893` and the injection loop `~:1024-1051`).
- The direct-path defer machinery as the semantic reference: `tur_frame`,
  `tur_frame_push_defer`, `tur_frame_fire_lifo` / `_fire_chain`
  (`src/compiler/emit_expr.c:1946-2058`, `src/runtime/runtime.h`).
- The control-op seed / liveness the hoist reasons over: `operand_uses_control`
  (`src/passes/cps_ir.c`), `cps_directly_uses_control` (`src/passes/cps.c`),
  `owning_dropped_before_control` / `letraw_ok` (`src/compiler/emit_cps_ir.c`).
- `CT_LETRAW` straight-line drop delegation (O1-a; `src/passes/cps_ir.c`) as the
  emit shape for the hoisted drop.
- DK teardown + owning-capture clone/drop (P2/P3):
  [cps-backend-multishot-continuations-owning-capture-plan.md](../archive/history/cps-backend-multishot-continuations-owning-capture-plan.md).

## Out of scope

- **General user `defer` in a colored function** -- arbitrary deferred effects
  under delimited control are a broader defer-semantics question. P1 gates to the
  elaborator-injected owning auto-drop shape only; a general `defer` keeps
  falling back.
- **Captured owning values in general** -- that is O3 / the env-capture plan; P3
  is only the `ref`-flavored instance of it.
- **Adding `ref` / `weak` / `lref` to `slot_ty`** -- Findings 1-2 of the parent
  stand: owning pointers never cross the slot bare, so there is nothing to hook.
