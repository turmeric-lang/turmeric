# fh-discharge-row: a `with-handler` that discharges one effect and leaves a LEFTOVER (RESOLVED)

**STATUS: RESOLVED.** `fh-discharge-row` DK-lowers (output `inner`, zero eff=1),
and all four already-DK-lowering with-handler mains STAY at zero eff=1
(fh-handler-value, fh-compose-handlers, fh-multishot-value, fh-multi-effect-type).
Net +1.  Flag-off byte-identical; flag-on effect soundness sweep clean; full
suite green (2203/0).

## The fix (a NARROW coloring signal + literal translation)

The first attempt (below) failed because coloring EX_WITH_HANDLER broadly
regressed the mains.  The resolved version colors ONLY the case that needs it:
- **cps.c** `cps_color_program`: color a fn iff its body contains a
  `with-handler` with a HANDLER LITERAL (`cps_body_has_with_handler` requires
  EX_HANDLER_LIT) AND the fn has a non-empty declared/inferred effect row
  (`cps_fn_has_leftover_effect`).  `do-work` (#fx{Other} leftover, literal
  handler) is colored; a with-handler main that discharges EVERYTHING (empty row)
  or uses a DYNAMIC handler (`run-with`'s `h`, a `compose-handlers`) is NOT --
  those keep their existing lowering, so no regression.
- **cps_ir.c**: `build_handle_core` (splits a HandleExpr's cases from its body)
  backs both `build_handle` and `build_with_handler`; `build_with_handler`
  translates `(with-handler <literal> body)` as a handle over body.  A dynamic
  handler still evicts (but is never colored, so never reached for those fns).

The two narrowings -- literal handler AND non-empty row -- each independently
exclude the regressed fixtures; together they make the change strictly additive.

## Original diagnosis + pitfall (retained)

**ATTEMPTED + REVERTED (first cut).** Coloring EX_WITH_HANDLER as a broad control
seed DID DK-lower fh-discharge-row, but REGRESSED four already-DK-lowering
fixtures -- see the pitfall below.  Reverted, then re-landed narrowly (above).

## The shape

```turmeric
(defeffect Write [s : cstr] : nil)
(defeffect Other [] : int)
(defn do-work [] #fx{Other} : int
  (with-handler
    (handler (Write [s] k) (do (println s) (resume k nil)))   ; discharges Write
    (do (perform (Write "inner")) (perform (Other)))))        ; Other is LEFTOVER
(defn main [] : int
  (handle (do-work)
    (Other [] k) (do (resume k 7) 0)))
```

`do-work` performs Write (handled locally by with-handler) and Other (leftover ->
must reach main's DK handler).

## Root cause

`do-work` is NOT colored: `cps_directly_uses_control` (src/passes/cps.c) has no
`EX_WITH_HANDLER` case, so a fn whose body IS a with-handler is never seen as
using control.  `do-work` therefore fiber-emits (`static int64_t do_hywork()`,
no `__cps`) and perm-taints `Other`, and `main` (the Other handler)
co-classifies SIG-TAINT.  The CPS translation (cps_ir.c) also lacks an
EX_WITH_HANDLER case (-> CT_UNSUPPORTED).

## The pitfall (why the naive fix is net-negative)

Coloring `EX_WITH_HANDLER` as a control seed is TOO BROAD: it also colors the
TOP-LEVEL with-handler mains that already DK-lower through the existing
machinery, disrupting them:

- `fh-handler-value`, `fh-multishot-value`: recovered by ALSO adding
  `EX_WITH_HANDLER` to `expr_has_handle` (so `fn_is_d2b_main` keeps a with-handler
  main d2b -- emit_cps_ir.c ~2557).
- `fh-compose-handlers`: STILL regresses -- its handler is `(compose-handlers h1
  h2)` (EX_COMPOSE_HANDLERS), which the literal-only `build_with_handler`
  translation does not cover -> CT_UNSUPPORTED.
- `fh-multi-effect-type`: STILL regresses -- `run-with` (a non-main with-handler
  fn) is SIG-REJECT (fn_sig_ok / a fn-value param), so coloring it forces a
  reject-eviction it did not have before.

Measured: the naive fix was +1 (fh-discharge-row) / -2 net even with the
expr_has_handle recovery.  All four fh mains DK-lower at baseline WITHOUT the
coloring, so the change only has downside for them.

## Fix direction (a real, careful slice)

Two independent pieces, both needed and both flag-gated:
1. **Color only the case that needs it, without disrupting the mains.**  Either
   restrict the with-handler control seed to NON-main leftover-discharge fns, or
   (cleaner) key coloring on "the with-handler body performs an effect the handler
   does NOT discharge" (a genuine leftover), leaving pure-discharge mains on their
   existing path.  Pair with `expr_has_handle` recognizing with-handler so a
   with-handler main stays d2b.
2. **Extend the CPS translation** beyond a single handler LITERAL: cover
   `compose-handlers` and a dynamic handler VALUE, and handle a with-handler fn
   whose signature currently SIG-REJECTs (`run-with`).  `build_handle_core`
   (the refactor that splits a HandleExpr's cases from its body) is the right
   substrate; `build_with_handler` must dispatch on EX_HANDLER_LIT /
   EX_COMPOSE_HANDLERS / a bound handler var.

## Verification recipe

- fh-discharge-row DK-lowers (output `inner`, zero eff=1) AND the four baseline
  fh mains (fh-handler-value, fh-compose-handlers, fh-multishot-value,
  fh-multi-effect-type) STAY at zero eff=1 -- net must be strictly positive.
- Flag-off byte-identical; FULL flag-on soundness sweep; full suite green.

## Context

One of the remaining Stage E residuals alongside `effect-nested`
(value-position nested handle), `effect-ref` (owning-ref across control),
`effect-capture-k` (by-ref mut capture).
