# E2 residual: a PURE lambda subtyped into an EFFECTFUL fn-value param evicts (effect-subtype cluster)

**STATUS: RESOLVED (verified 2026-07-18).** The whole effect-subtype cluster now
DK-lowers under `--enable=cps-tramp-resume`: `effect-subtype-ho`,
`effect-subtype-assign`, `effect-subtype-capability`, `effect-type-alias`, and
`effect-struct-field-row` all emit zero `tur_effect_perform("` call sites with
the flag on (measured). On the fiber path (no flag) they still ride the fiber by
design (not opted in), which is the expected pre-graduation state. Archived from
`docs/reported/`.

(Historical status below.)

**STATUS: PARTIALLY LANDED (2 of 6).** effect-subtype-ho + effect-subtype-assign
DK-lower via the two-part fix (force-color the pure lambda in cps.c + un-skip a
colored pure fn-value in the E2 registration loop, emit_cps_ir.c).  The remaining
4 (effect-type-alias, effect-struct-field-row, capability-effect-poly,
fh-discharge-row) have a DISTINCT residual (still SIG-TAINT, correct output) --
the lambda there is likely not a bare-EX_VAR lifted node the force-coloring peel
reaches (inline / boxed differently), or the HOF call chain differs.  Flag-off
byte-identical; effect family flag-on == baseline.


**Severity:** low-medium (correct on the fiber; endgame migration target).  The
effect-subtype/-type-alias/-struct-field-row cluster (~6 fixtures) stays SIG-TAINT
under `--enable=cps-tramp-resume` AFTER the E2 row-poly cluster landed
(commit 4b8ea6b).  Root: effect SUBTYPING -- a PURE lambda passed where an
EFFECTFUL fn-value is expected.

## The shape (pinned)

```turmeric
(defeffect Write [msg : cstr] : nil)
(defn run-twice [f : (fn [] #fx{Write} nil)] #fx{Write} : nil (do (f) (f)))
(defn main [] : int
  (handle (do (run-twice (fn [] (println "ok"))) 0)          ; PURE lambda, no perform
    (Write [msg] k) (do (println msg) (resume k 0))))
```

`run-twice`'s param `f` has a CONCRETE `#fx{Write}` row, so `param_thread_class`
tiers it a thread-param and `run-twice` threads `f`'s calls via the registry
(`__tur_cps_lookup(f)(args, __kont)`).  But the lambda `(fn [] (println "ok"))` is
PURE -- it does not perform `Write` (effect subtyping: a pure fn is a subtype of
an effectful fn-value).

**Verified isolation:** the SAME `run-twice` with an EFFECTFUL lambda
(`(fn [] (perform (Write "x")))`) threads and prints correctly (`do`/`+`, tail or
non-tail all work post-4b8ea6b).  Swapping in the pure lambda evicts
(`run-twice`/`main` SIG-TAINT, eff=1=2).  No `[E2-COLOR]` line appears for the
pure lambda -- the E2 threadability analysis only considers EFFECTFUL fn-values
(the registration loop, `emit_cps_ir.c` ~3599, skips `!(lo||hi)`), so the pure
lambda is never `threadable_add`ed.

## Root cause

A pure lambda is NOT `cps_colored` (its body uses no control op and calls no
colored fn -- `cps_color_program`, src/passes/cps.c), so it gets NO `__cps` entry
and is never registered.  When `run-twice` threads `f` via the registry,
`__tur_cps_lookup(<pure-lambda-fn-ptr>)` finds no `__cps` -> the threaded call has
no entry to dispatch to -> `run-twice` cannot thread -> evicts -> its effect taints
-> `main` (handler) co-classifies SIG-TAINT.

The effectful lambda works because it IS colored (its body performs) -> CPS-emitted
-> registered.  The difference is purely the callback's BODY, not its use.

## Fix direction (force-color pure lambdas that flow into effectful fn-value params)

A lifted lambda passed as an argument to an EFFECTFUL `TY_FN` param must get a
`__cps` entry + registration, EVEN IF its own body is pure -- so the HOF's
registry thread finds it.  Its `__cps` is trivial: run the (pure) body, deliver
the result to `__kont`.

Implementation options:
1. **Forward-coloring step in `cps_color_program`** (src/passes/cps.c), gated on
   `g_opt_cps_tramp_resume`: color a lifted lambda passed to an effectful `TY_FN`
   param.  **ATTEMPTED + INSUFFICIENT (2026, reverted):** a flag-gated
   `cps_force_color_eff_fnval_args` forward pass was added and correctly compiled
   (flag-off byte-identical), but the fixtures did NOT move -- coloring the lambda
   alone is not enough.  Two further gaps found:
   - The E2 registration loop (`emit_cps_ir.c` ~3599 `if (!(lo||hi)) continue;`)
     SKIPS a pure fn-value, so even a colored pure lambda is never
     `threadable_add`ed -> the HOF's `__tur_cps_lookup` still misses.  The loop
     must ALSO register a pure lambda that is colored AND flows into an effectful
     param.
   - `run-twice` is SIG-TAINT (not BODY-*) even before threading -- some source
     PERMANENTLY taints `Write` ahead of run-twice.  Pin that seed
     (`g_perm` dump) before assuming coloring+registration suffices; the pure
     lambda used as an effectful fn-value may be force-classified `sig_perm`
     somewhere (the address-taken/lifted-lambda path) despite being pure.
   So option 1 is a THREE-part change (color + register + un-taint), each verified.
2. **Registration-only wrapper** (heavier): synthesize a `__cps` threading wrapper
   for a pure fn-value flowing into an effectful param and register the wrapper.
   More emission surface; option 1 is cleaner once the three gaps are closed.

## Affected fixtures (measured, all SIG-TAINT, correct output on fiber)

`effect-subtype-ho` (ok), `effect-subtype-assign` (pure), `effect-type-alias`
(type alias test), `effect-struct-field-row` (struct field row),
`capability-effect-poly` (world), `fh-discharge-row` (inner).  (`effect-poly-map`
is a DIFFERENT residual: non-tail leaf-fiber recursion.)

## Verification recipe

- Force-color per option 1; target: the 6 fixtures emit their HOF `__cps` with
  zero `eff=1` and unchanged output.
- Flag-off byte-identical (default suite green; the coloring addition must be
  flag-gated).
- FULL flag-on soundness sweep (every fixture flag-on == flag-off baseline) --
  mandatory for this cluster.

## Context

Stage E residual after the row-poly cluster
(docs/archive/cps-e2-rowpoly-fnvalue-threading-boundary.md).  Complements E2b
(pure-fn-value into effect-FREE callee, task PK) -- this is the converse: a pure
fn-value into an effect-FUL callee that threads.
