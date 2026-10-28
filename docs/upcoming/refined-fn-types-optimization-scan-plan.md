# Plan: Optimization scan of calls through function values and their checks

> **Status:** TODO (noted 2026-10-28). Nothing scanned yet. A follow-up to
> [refined-function-types-plan](refined-function-types-plan.md)'s F4, which
> measured the call-site checks and kept them. Off the v1 critical path.
> **Type:** Performance / investigation
> **Output:** a `docs/reported/<slug>.md` per finding worth fixing (with a row
> in `docs/reported/README.md`), or a line here saying the candidate came up
> empty. Fixes are not part of this plan.

## Why

F4 timed the refinement checks on a call through a function value and found
them free next to the call. What it found instead is that the *call* is
expensive. As emitted today, a call through a function value costs about 70 ns,
against about 1.5 ns for the same indirect call in plain C
([benchmarks/refined-fn-result-check-results.md](../../benchmarks/refined-fn-result-check-results.md)).
That is the wrong place to stop looking. This plan is the note to come back and
scan the area properly.

## Scope

The code a call through a function value, and the refinement work around it,
goes through:

- **Elaboration.** `rt_elab_refined_fn_call`, `rt_resolve_fn_type_crossing`,
  `rt_check_fn_coercion` / `rt_decide_entailment`, and the local-function
  resolver `rt_resolve_fn_with_locals` / `rt_env_add_local_fns`
  (`src/compiler/elab_fns.c`). Also the hooks in `elab_call.c`,
  `elab_forms.c` and `elab_types.c`.
- **Emission.** The CPS colouring of indirect calls (`src/passes/cps.c`), and
  what a coloured higher-order loop pays on the DK machinery
  (`emit_cps_ir.c`).
- **The interpreter's contract checks** (`tur-contract-check-at` under
  `tur --interpret`).

## Candidates to check first

Seeded from what F4 ran into. Each is a lead, not a finding: measure before
filing.

1. **Indirect calls are CPS-coloured unconditionally (R-1).** This is already
   ranked: item 17 in
   [the perf audit summary](../artifacts/perf-audit-2026-10/summary.md) ("R-1
   properly: effect rows for function values"). Do not re-file it. What is
   new: a function value whose type declares an empty effect row
   (`(fn [int] #fx{} int)`) is coloured anyway. That is the narrow first step
   R-1 needs, and is worth its own report if nothing already covers it.
2. **The interpreter's contract check costs 0.3--0.5 us a call** and roughly
   doubles a trivial loop. A hand-written `(if p x (panic ...))` is about 25%
   cheaper than the `tur-contract-check-at` call it replaces. Every contract
   pays this, not only refined function types. Find out where the time goes:
   the call itself, building the message, or the -at helper's extra
   arguments.
3. **Compile-time cost of the refinement work per call site.** Things to time
   on a program with many calls through refined function values:
   - `rt_decide_entailment` builds a fresh `RefineEnv` per check;
   - the same (function, function type) pair is decided again at each site;
   - the coercion dedup (`rt_fn_coercion_first`) is a linear scan;
   - `rt_form_binds_name` walks the whole body once per refined
     function-typed parameter.
   Each is cheap alone. Check whether any shows up in `--timings`.
4. **The call-site wrapper's temporaries.** `rt_elab_refined_fn_call`
   elaborates a non-trivial argument on its own to bind it to a temporary, so
   that argument loses the expected type its parameter would have pushed. The
   question is whether this ever costs a conversion or a box in the emitted C,
   compared with the same call through an unrefined type.
   `tests/check-refined-fn-types-erased.sh` only compares with contracts off.

## How

- Start from the F4 benchmark (`benchmarks/bench-refined-fn-result-check.sh`)
  and `--timings`. Add a case to the benchmark rather than a one-off script
  when a candidate needs one.
- `TUR_CPS_CENSUS=1 tur emit-c` attributes colouring (cps-dk-internals-guide,
  "Over-coloring").
- Compare against `main`'s numbers, measured on the same box in the same
  session. Two full-size runs differed by 7% during F4, so treat anything
  below that as noise.

## Done when

Each candidate above has either a filed report or a one-line "measured, nothing
worth fixing" entry under it, plus anything else the scan turns up. Then move
this file to `docs/archive/`.
