# A `perform` continuation containing a non-tail cps->cps call (heap join) evicts

**RESOLVED** -- fixed as part of the `effect-reopen` DK-admission slice; see
`docs/archive/history/cps-perform-cont-heap-join-eviction.md`.  The heap-join
half is `jbody_has_perform` (`emit_cps_ir.c`): `emit_heap_join` now lifts a
join whose jbody itself PERFORMS as an `LH_RESUME_CONT` resume-frame (which
carries `__kont`) instead of a value-only `LH_PERFORM_CONT` frame (which has no
`__kont`, so the interior `dk_perform` referenced an undeclared `__kont`).  Two
companion admissions were needed for the full `effect-reopen` shape: `println`
in a perform continuation (`perform_body_ok`/`perform_cont_reset_ok`), and a
handle-group id (`dk_hgroup`) so effect RE-OPENING across a nested handle with a
multi-suspension continuation dispatches correctly.  All gated on
`--enable=cps-tramp-resume`; `effect-reopen` now DK-lowers (no `eff=1` eviction)
and prints `start`/`done`/`142`.  Regression fixture
`tests/fixtures/cps-tramp-resume-reopen`.

**Severity:** low-medium (blocks the CPS/DK admission of the compound
`effect-reopen` fixture and any `perform; let x = <colored-callee>; ...` shape;
correctness is fine -- the function runs on the fiber).  Not a shipping-backend
regression -- pre-existing, independent of effect re-opening.

**One-line:** a `perform` whose continuation makes a NON-TAIL call to a colored
(cps->cps) callee -- i.e. `perform E; let x = f(...); <use x>` where `f` is
CPS-emitted -- is not admitted: neither `perform_body_ok` (rejects the
`CT_LETCONT` heap-join) nor `perform_cont_reset_ok` covers the reified-join
shape, so the enclosing function evicts as `BODY-STRUCT-OR-TAINT`.

## Minimal repro

```turmeric
(defeffect Counter [] : int)
(defn counted-sum [n : int] : int (+ (perform (Counter)) n))
(defn main [] : int
  (handle
    (do (perform (Counter))                         ; <-- a perform, whose continuation ...
        (let [result (counted-sum 100)]             ; ... makes a NON-TAIL cps->cps call
          (do (println result) 0)))                 ; ... and then transforms the result
    (Counter [] k) (resume k 42))
  0)
```

`main` evicts `[EVICT] BODY-STRUCT-OR-TAINT eff=1 main` (and `counted-sum` with
it, via shared-effect taint) under `--enable=cps-tramp-resume`.  Removing the
leading `(perform (Counter))` -- so the non-tail call is no longer in a perform
continuation -- admits it (a heap join at the top level of a handled body is
fine).  Making the call TAIL (`... result))` instead of `(do (println result)
0)`) also admits it.  So the trigger is specifically a heap-reified join sitting
in a `perform` continuation.

This is the second half of the `effect-reopen` fixture's eviction: its inner
body is `perform Log "start"; let result = counted-sum(100); perform Log "done";
println result; 0`.  The re-opening half is resolved
(`docs/archive/cps-handler-case-effect-reopening-needs-emission.md`); this
heap-join-in-perform-continuation half remains, so the fixture still routes to
the fiber (it runs correctly: `start`/`done`/`142`).

## Why

`emit_perform` lowers a non-trivial continuation via `perform_body_ok`
(straight-line `LH_PERFORM_CONT`) or the Track-A `perform_cont_reset_ok`
(`LH_RESUME_CONT`).  A non-tail cps->cps call is represented as a `CT_LETCONT`
whose body tailcalls the colereced callee threading a reified join
(`needs_heap_join` / `emit_heap_join`).  `perform_body_ok` has no `CT_LETCONT`
case (rejects), and `perform_cont_reset_ok`'s `CT_LETCONT` case only admits a
join whose `jbody`/`body` are themselves `perform_cont_reset_ok` -- it does not
model a join that TAILCALLS a colored callee (the `emit_heap_join` shape).  So
the perform continuation is rejected and the whole function evicts.

## Fix direction

Teach the perform-continuation lowering to reify a heap join the way a normal
(non-perform) body already does.  Concretely: admit a `CT_LETCONT`-with-cps->cps-
tailcall in the perform continuation (extend `perform_cont_reset_ok`, or route
the perform continuation through the same `needs_heap_join` / `emit_heap_join`
machinery that `emit_term`'s top-level `CT_LETCONT` uses), threading the perform
frame's runtime `__kont` as the join frame's downstream chain so the callee's
result flows back through the join and is delivered exactly once.  Verify with
the repro above (expect `142`) + the `effect-reopen` fixture reaching full CPS
admission (no `[EVICT]` for `main`/`counted-sum`) + suite.
