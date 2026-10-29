# Plan: Optimization scan of calls through function values and their checks

> **Status:** DONE (scanned 2026-10-28); see "Outcome" below. A follow-up to
> [refined-function-types-plan](../upcoming/refined-function-types-plan.md)'s F4, which
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

## Outcome (2026-10-28)

Every candidate has a verdict, and the scan found four things outside its
list. Six reports filed, indexed under "Found by the refined-fn-types
optimization scan" in `docs/reported/README.md`.

1. **CPS colouring of a `#fx{}` parameter.** Confirmed: a parameter typed
   `(fn [int] #fx{} int)` is coloured by the indirect rule alone
   (`TUR_CPS_CENSUS=1`: `colored indirect`). Not filed as an optimization,
   because uncolouring it is unsound today: a function that performs `IO`
   reaches a `#fx{}` slot through a `let`
   ([effect-row-check-skips-let-bound-fn-values](effect-row-check-skips-let-bound-fn-values.md)).
   That is the precondition R-1's fix already names. Found next to it:
   [fn-param-not-forwardable-to-effect-rowed-param](../reported/fn-param-not-forwardable-to-effect-rowed-param.md).
2. **The interpreter's contract-check cost.** Filed:
   [turi-contract-check-is-a-full-native-call](../reported/turi-contract-check-is-a-full-native-call.md).
   A check costs a full native call (+0.21 us, +0.29 us for the `-at` form)
   against +0.10 us for an inline `if` + `panic`.
3. **Compile-time cost of the refinement work.** Measured, nothing worth
   fixing. On generated programs with 250 and 1,000 higher-order functions:
   - refining their function types costs about 1.4x the unrefined compile;
   - the `refine` phase (x3.8) and elaboration (x3.6) grow linearly for 4x
     the input;
   - no part of the refinement work is quadratic: the coercion dedup,
     `rt_decide_entailment`'s fresh environments and `rt_form_binds_name`
     never showed in a profile.
   The superlinear-looking emission time came from two other places. One is
   [cps-fn-unreached-rescans-all-inline-c](cps-fn-unreached-rescans-all-inline-c.md)
   (filed, low: about 0.5 ms per unreferenced coloured function, invisible on
   the corpus). The rest is cache effects: instruction counts grow linearly.
4. **The call-site wrapper's temporaries.** Measured, nothing worth fixing. A
   temporary is typed like the argument and adds no conversion or box. The
   only difference in the emitted C is the check itself. Probing it found a
   much larger bug in the unrefined path:
   [call-through-fn-param-skips-argument-checks](call-through-fn-param-skips-argument-checks.md)
   (high). A call through a plain function-typed parameter checks neither
   argument types nor arity. The refined path rejects such a call only by
   accident, with a misleading `TUR-E0042`.

Also found while checking candidate 4:
[float-conversion-check-ignores-fixture-flags](float-conversion-check-ignores-fixture-flags.md).
The float-conversion mechanism check never sees an experiment-gated fixture,
the refined-fn-types ones included.

Archived 2026-10-28.

