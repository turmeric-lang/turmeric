---
title: Routing carrier <-> concrete crossings through shared recovery chokepoints -- retiring the per-site patch pile
category: Planning -- ABI / Codegen (carrier<->concrete unification, structural)
description: The crossing audit (docs/archive/carrier-concrete-abi-crossing-audit-plan.md -- archived 2026-06-22, all gaps closed)
  converted an open-ended bug stream into a closeable list, but each gap is
  still being closed the way the prior 45 were -- a narrowly-gated branch added
  next to the last one. This plan is the structural counterpart: make the two
  shared recovery routines MANDATORY chokepoints every crossing passes through,
  so a new emit site is correct by construction instead of being the next fish.
  It exists so the unification work is not lost between one-off gap closures.
status: RESOLVED -- archived 2026-06-23. All R0-R4 structural pieces
  landed; the actionable dispatch-side collapse is done and the only
  remaining items are by-design exclusions documented in the inventory.
---

# Routing carrier <-> concrete crossings through shared recovery chokepoints

## Status (RESOLVED, archived 2026-06-23; structural status verified post-PR #505)

**LANDED.** All R0-R4 structural pieces are in place: the value,
dispatch, and fn-value axes each have a single named chokepoint, the
value-side ad-hoc copies are migrated, the dispatch-side constraint-var
mapping is collapsed onto the shared `emit_abi_constraint_var_bindings`
kernel, the R3 Debug-only ICE is wired, and R4's static registry + CI
check ship in `tests/run.sh`. The only remaining items are by-design
exclusions called out in the inventory (the `emit_abi_register_call`
`abi_changes` arg/result block and the fn-value inner-clone derivation),
which would change behavior if merged -- so this plan is closed.

- **R0 -- Inventory.** DONE for value side; dispatch and fn-value axes
  inventoried with by-design exclusions documented. Tables below match
  current code.
- **R1 -- Chokepoints total.** DONE for the dispatch-tyvar identification
  walk: `emit_dispatch_tyvar` at `src/compiler/emit_core.c:1241`
  (decl `emit_internal.h:398`), consulted by `emit_reresolve_disp_type`
  (`emit_core.c:1271`) and `emit_call_dispatches_on_spec_tyvar`
  (`emit_module.c:1159`). Constraint-var tail vs.
  `emit_ground_constraint_var` deliberately not merged (would change
  instance selection).
- **R2 -- Migrate ad-hoc sites.** Value side DONE in commit `ae0d1ad5`
  (PR #505): `emit_spec_arg_type_for_binding` at `emit_expr.c:1641`
  (decl `emit_internal.h:407`); `emit_var_spec_arg_type`
  (`emit_expr.c:1662`) delegates. Migrated callers verified:
  `field_read_emits_byvalue_aggregate` (`emit_expr.c:417`),
  `emit_reresolve_disp_type` field-receiver tail (`emit_core.c:1332`),
  M4c Path A.2 override (`emit_expr.c:5338`), instance-method twin-arg
  resolver (`emit_module.c:1841`). Registry shows 6 call sites across
  emit_core/emit_expr/emit_module. **Dispatch side advanced:** the
  constraint-var `param_idx`->element mapping that
  `emit_reresolve_disp_type`'s tail and `emit_abi_register_call`'s
  binding augmentation each open-coded is now a single shared kernel,
  `emit_abi_constraint_var_bindings` (`emit_core.c`, tracked chokepoint;
  registry rows `emit_core.c 1` + `emit_module.c 1`). Each caller keeps
  its own receiver extraction (struct-strict vs. any-TY_APP) -- that
  difference is load-bearing for instance selection -- so only the
  mapping collapsed; snapshot-stable (1765/0). **Still open:** the
  remaining `emit_abi_register_call` `abi_changes` arg/result block
  (excluded by design) and the per-bug recovery branches *inside*
  `emit_reresolve_disp_type` (these are the R1-totalized routine body,
  legitimately internal, not call-site duplication).
- **R3 -- Debug "forgot to route" ICE.** DONE.
  `emit_abi_assert_routed_concrete` at `src/compiler/emit_core.c:141`
  (decl `emit_internal.h:415`), called from value chokepoint
  (`emit_expr.c:1654`) and dispatch chokepoint (`emit_core.c:1448`).
  Registry confirms 2 call sites. `TUR_ABI_NO_ROUTE_ICE=1` escape hatch
  present. Strictness calibrated per-side (value: bare `TY_TYVAR` only;
  dispatch: whole resolved spine tyvar-free).
- **R4 -- Audit registry + CI gate.** DONE.
  `docs/artifacts/crossing-routing-audit.txt` (11 routine/file rows) and
  `tools/check_crossing_routing.py` exist; wired into `tests/run.sh:48`
  behind `TUR_SKIP_PARITY_CHECK=1` opt-out.
- **Fn-value third axis.** STARTED. `emit_abi_fn_value_signature` at
  `src/compiler/emit_module.c:3381`, called from
  `emit_abi_scan_fn_values` (`emit_module.c:3438`). Inner-clone
  derivation and `emit_abi_register_call` arg/result block remain
  by-design unrouted.

**Open items not yet addressed by R0-R4:**

1. Dispatch-side R2: the constraint-var `param_idx`->element mapping is
   now collapsed onto the shared `emit_abi_constraint_var_bindings`
   kernel (both `emit_reresolve_disp_type`'s tail and
   `emit_abi_register_call`'s augmentation route through it). What
   remains is the `emit_abi_register_call` `abi_changes` arg/result
   block, which the R0 inventory marks excluded by design (layered M4c
   Path A.1 + G4 phantom + borrow-path cases; sharing would change
   behavior).
2. End-state per the "Validation" section: per-bug gated branches gone
   and audit table free of `[GAP]` rows -- not yet realized.
3. Suite status: PR #505 reported 1765 passed / 0 failed.

Recent relevant commits: `ae0d1ad5` (R2 value-side migration),
`3a8e4df7`, `5332f0c1`, `738fe342`, `52b8823e` (gap-closure PRs the
plan was filed against).

### R0 inventory -- value-side recovery (`params[pi] -> arg_types[pi]`)

| Site | Status | Note |
|---|---|---|
| `emit_expr.c` `emit_var_spec_arg_type` | ROUTED (is the chokepoint; now delegates to `emit_spec_arg_type_for_binding`) | -- |
| `emit_expr.c` monomorphized-ctor arg slot bridge (`macos-int-conversion-carrier-pointer-straddles` case A) | ROUTED | added 2026-08-01; resolves a bare arg var through the active spec so its emitted C type can be compared against the ctor param C type recorded in the signature side table |
| `emit_expr.c` `expr_is_erased_carrier_param` (SR2a graduation) | ROUTED | added 2026-08-27; the stale-flag guard -- `emit_carrier_holds_byval` is set while emitting the generic instance BASE and persists into a by-value spec emission, where the param really is the aggregate |
| `emit_expr.c` `call_arg_spill_type` / `arg_is_spec_byvalue_param` (SR2a graduation) | ROUTED | added 2026-08-27; a spec param that is a by-value monomorph feeding a carrier-typed generic base -- the spec's arg type is what the spill temp must be declared at, and asking the argument's static type instead is the repr-shadow ICE at `arg-bridge` |
| `emit_expr.c` match-scrutinee spec narrowing (SR2a graduation) | ROUTED | added 2026-08-27; resolution can ground a scrutinee param's element from a DIFFERENT instantiation than the active spec passes (`ap`'s `ff`), which only shows once the two spellings stop both being `int64_t` |
| `emit_expr.c` call-arg opaque-pointer spelling relabel (opaque-pointer-c-spelling) | ROUTED | added 2026-08-28; a pointer `defopaque` and the int64 carrier are the same word under two names, so a formal and an argument can disagree about the spelling -- the argument's own type is asked through the active spec so the relabel fires on the value, not on an ascription that was stripped before emission |
| `emit_expr.c` generic-base ctor arg slot (opaque-pointer-c-spelling) | ROUTED | added 2026-08-28; the un-suffixed base ctor is absent from the ctor signature side table, so the case-A straddle bridge above had no slot type to compare against -- the spec resolves the argument to name the base's carrier slot for a pointer-opaque argument |
| `emit_expr.c` `field_read_emits_byvalue_aggregate` (G9) | MIGRATED | inline copy deleted |
| `emit_core.c` `emit_reresolve_disp_type` field-receiver recovery | MIGRATED | inline copy deleted |
| `emit_expr.c` M4c Path A.2 by-value field-access override | MIGRATED | inline copy deleted |
| `emit_module.c` instance-method twin-arg resolver (Gap 1) | MIGRATED | inline copy deleted |
| `emit_fns.c:910` carrier-payload param index | EXCLUDED | wants the param *index*, not its type -- different routine |
| `emit_effects.c:211` `ctx->fn_params[pi]` | EXCLUDED | different array (effect-handler params), not spec `arg_types[]` |

### R0 inventory -- dispatch-side recovery

| Site | Status | Note |
|---|---|---|
| `emit_core.c` `emit_reresolve_disp_type` dispatch-tyvar identification | ROUTED (delegates to `emit_dispatch_tyvar`) | -- |
| `emit_module.c` `emit_call_dispatches_on_spec_tyvar` tyvar identification | MIGRATED | inline copy deleted; now calls `emit_dispatch_tyvar` |
| `emit_module.c` `body_has_dispatch_on_app_tyvar` receiver-less return dispatch | ROUTED | added 2026-10-12; a nullary return-directed method on an abstract tyvar (`(:: (dflt) A)`) has no receiver for the probe's other tests, so its dispatch tyvar is asked through `emit_dispatch_tyvar` (which maps an associated-type projection result to its argument) to decide whether this binding mints a spec |
| `emit_core.c` constraint-var resolution tail (`param_idx`->elem) | MIGRATED | param_idx->element mapping routed through shared `emit_abi_constraint_var_bindings`; struct-strict extraction stays site-local |
| `emit_module.c` `emit_abi_register_call` constraint-binding augmentation | MIGRATED | same `param_idx`->element kernel; any-TY_APP extraction stays site-local |
| `emit_core.c`/`emit_module.c` constraint-var vs. `emit_ground_constraint_var` | NOT MERGED (by design) | `emit_ground_constraint_var` differs (param_idx<0 + concreteness gating); merge would change instance selection |
| `emit_module.c:2164` scan-time dispatch recovery | ALREADY ROUTED | calls `emit_reresolve_disp_type`; redirect helpers consume its result |
| `emit_expr.c` return-dispatched sum-cell drain type (value-struct-payload round 4) | ROUTED | added 2026-09-03; a stamped `(:: (dec tag) (Result A cstr))` producer carries the abstract class var as its own type, so the pending free could not see the boxed arm -- the re-resolved instance's declared result with the class var substituted by the dispatch type (an owned spine, freed at the drain) is the cell's real layout; never resolved through the active spec, which binds that var to the OUTER receiver |

### R0 inventory -- fn-value (closure-thunk) axis

| Site | Status | Note |
|---|---|---|
| `emit_module.c` `emit_abi_scan_fn_values` signature derivation | ROUTED (delegates to `emit_abi_fn_value_signature`) | the named third-axis chokepoint |
| `emit_module.c` poly-closure inner-clone derivation (Stage B+C) | NOT ROUTED (by design) | env-aware (param 0 = env ptr); specialize decision already made upstream, computes no `abi_changes` |
| `emit_module.c` `emit_abi_register_call` arg/result `abi_changes` block | NOT ROUTED (by design) | layered M4c Path A.1 + G4 phantom + borrow-path cases; sharing would change behavior |

## Why this plan exists (read first)

`docs/archive/carrier-concrete-abi-crossing-audit-plan.md` (archived 2026-06-22, every gap closed) did the hard diagnostic
work: it named the **one defect** (a parametric payload riding the int64
carrier where a concrete representation was required), enumerated the crossing
sites, and named the **two shared recovery routines** they should all consult.

What it did *not* do is make those routines a **gate**. They are invoked **ad
hoc, one emit site at a time**. So every gap (G1..G7) is still closed the way
PRs #437-#481 were -- by adding another narrowly-gated branch next to the last
one. `emit_module.c`'s `emit_abi_register_call` and `emit_core.c`'s
`emit_reresolve_disp_type` are already stacks of such branches, each commented
with the single bug it closed (`Gap #4`, `M5 residual-straddle`, `GHE2`,
`WKC3`, `Option C`, ...). A new contributor (human or agent) pattern-matches
that style and ships branch N+1. The architecture reproduces point-patches
because it *is* a pile of point-patches.

This plan is the antidote: turn the two recovery routines into **mandatory
chokepoints**, migrate the existing ad-hoc sites onto them, and delete the
redundant branches. After that, a crossing that forgets to route is a **visible
hole** (a missing audit row / a failing chokepoint assertion), not a silent
miscompile waiting for a spice to find it.

## The two chokepoints

Every carrier<->concrete crossing is on one of two sides. Each side gets one
routine that is the *only* sanctioned way to recover the concrete type:

1. **Value side -- `emit_var_spec_arg_type` (`emit_expr.c`).** Given an
   expression whose declared type is a spec parameter (receiver `x : (Vec A)`)
   but whose active specialization is concrete (`(Vec int)`), return the
   concrete monomorphized type from `current_abi_specialization->arg_types[]`.
   Every emit context that types a value crossing the boundary
   (`EX_GET_FIELD`, `EX_ASCRIBE`, `EX_CALL` arg typing, ...) must obtain the
   element type from here, never from a bare `emit_resolve_type` that can leave
   a parametric param unsubstituted.

2. **Dispatch side -- `emit_reresolve_disp_type` (`emit_core.c`).** Given a
   typeclass-method call inside an active spec whose receiver/result element was
   erased to the carrier, recover the concrete dispatch type and select the
   correct concrete/per-instantiation `__inst_*`. Every method-dispatch emit
   site (call-name rewrite, scan-time liveness, the by-value spec mint G2 needs)
   must consult this -- and it must be **recursive** (nested parametric element)
   and **ascription-aware** (a field/result ascription naming the instance type
   wins over the enclosing spec's result type).

The gaps map cleanly onto the two sides:

| Side | Routine | Gaps |
|---|---|---|
| Value | `emit_var_spec_arg_type` / call-site materialization (`expr_emits_byvalue_carrier_abi` + `emit_carrier_bridge`) | G5 (Option field read / type ordering), G4 (consumer-side cons walk), **G3 DONE** (by-value field-read receiver bridged to the carrier at the call site) |
| Dispatch | `emit_reresolve_disp_type` | **G2 DONE** (nested parametric element), G7 (sum-typed field instance selection), G9 (witness-side mirror of G2) |
| Fn-value | (new, parallel axis) | G6 (closure-thunk per carrier + cata result) |

(G3 landed on the value/materialization side, not the dispatch re-resolver: the
instance method kept its uniform carrier ABI and the by-value field-read
*receiver* was bridged to the carrier at the call site -- the same path a
by-value local already used. G10, the applied-struct instance-selection
conflation, is a keying/mangling defect outside both chokepoints.)

G6 reveals a **third axis** the audit's value-centric framing missed: function
pointer / closure-thunk ABI. It needs its own chokepoint (the
`emit_abi_scan_fn_values` / `poly-closure-result-specialization` path), held to
the same discipline: the thunk signature follows the carrier `B`, never the
int64 default.

## Phases

### R0 -- Inventory (cheap, do first)

Grep every site that recovers a concrete element type *without* going through a
chokepoint -- a bare `emit_resolve_type` on a receiver/field, a hand-rolled
`arg_types[pi]` lookup, a re-mint of an `__inst_*` name. Add each as a row to
the audit table (file:line, side, routed? Y/N). This is the worklist; it is
also the P3 regression guard seeded.

### R1 -- Make the chokepoints total

Audit `emit_var_spec_arg_type` and `emit_reresolve_disp_type` for the cases the
gaps prove they miss, and fold those *into the routine* rather than at the call
site:

- `emit_reresolve_disp_type`: recurse when the recovered receiver type is itself
  a parametric container (G2); honor a field/result ascription that names the
  instance type over the enclosing spec result (G7); handle a by-value
  struct-field receiver (G3).
- `emit_var_spec_arg_type`: cover the Option field read as an embedded aggregate
  (G5 Site 1), and the `:heap` cons whose head is a by-value aggregate (G4).

The test for "is this in the routine yet": a *new* emit site that calls the
routine gets the right answer with **zero site-local special-casing**.

### R2 -- Migrate existing ad-hoc sites onto the chokepoint

Replace each gated branch found in R0 with a single chokepoint call. Expect this
to **delete** code: the stacked special cases in `emit_abi_register_call` /
`emit_reresolve_disp_type` that re-derive the same concrete type collapse into
one call each. Do it in small, snapshot-stable steps (one site per commit,
regenerate fixtures in the same commit per CLAUDE.md).

### R3 -- Make routing mandatory (the gate)

Add a debug-build assertion / invariant: a carrier-ABI value or method-dispatch
that reaches code emission with an unresolved parametric param type, *without*
having passed a chokepoint, is a hard `tur` ICE in Debug (not a silent
miscompile). This is what flips "forgot to route" from a downstream spice crash
into a local, immediate failure. Keep it Debug-only so release builds are
unaffected.

### R4 -- Audit-as-regression-guard (adopt the audit's P3)

The audit table is the single source of truth for "which crossings are routed."
Any PR adding a recovery call site adds a row + a stress-matrix cell. A crossing
without a row fails review. This plan's R3 assertion is the runtime enforcement
of R4's documentation.

**Landed.** The single source of truth is now machine-readable and enforced:

- `docs/artifacts/crossing-routing-audit.txt` -- the registry, one
  `<routine> <file.c> <call-site-count>` line per routed crossing.
- `tools/check_crossing_routing.py` -- enumerates the chokepoint call sites in
  `src/compiler/*.c` (definitions and prototypes excluded) and diffs them
  against the registry; `--update` regenerates it.
- `tests/run.sh` runs the check (with `--quiet`) alongside the turi-parity
  ratchets, so a crossing that forgot its row fails the suite, not just review.
  Opt out with `TUR_SKIP_PARITY_CHECK=1`.

The sanctioned chokepoints it tracks: `emit_spec_arg_type_for_binding` /
`emit_var_spec_arg_type` (value), `emit_reresolve_disp_type` /
`emit_dispatch_tyvar` / `emit_abi_constraint_var_bindings` (dispatch),
`emit_abi_fn_value_signature` (fn-value), and
`emit_abi_assert_routed_concrete` (the R3 gate the call sites lean on).

## Risks / why this wasn't done already

- **Snapshot churn.** Collapsing branches changes emitted C; many
  `tests/fixtures/*/expected.c` regenerate. Mitigate with R2's one-site-per-
  commit cadence and same-commit regen (CLAUDE.md fixture policy).
- **Legacy carrier suite.** Per CLAUDE.md the default by-value suite is the
  gate; the `TUR_M7_HKT=0` carrier path may degrade as sites migrate -- that is
  expected, not a regression to chase.
- **Each migration is locally riskier than a patch.** That asymmetry (cheap
  patch vs. risky refactor, under a single failing test) is *exactly* why 45
  sessions chose the patch. R2's small steps + R3's assertion are what make the
  refactor's risk bounded and its payoff durable.

## Validation

- `bash tests/run.sh` (10-minute timeout) green throughout; intermediate
  snapshot churn reconciled within each migration commit.
- Each gap (G2-G7) closes by *adding to a chokepoint* (R1) rather than adding a
  site-local branch; its stress-matrix cell promotes to a fixture and its report
  moves to `docs/archive/`.
- End state: `emit_reresolve_disp_type` and `emit_var_spec_arg_type` are each a
  single routine every crossing calls, the per-bug gated branches are gone, and
  the audit table has no `[GAP]` rows.
