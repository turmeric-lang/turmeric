# A threaded fn-value called in a non-tail `if` / `match` arm returns from the whole function

> Status: resolved 2026-11-28, filed and fixed together. The call's join is
> now reified as a resume-frame and threaded, so a pure and an effectful
> fn-value both give the right answer.

**Severity:** high (silent wrong answer, then a segfault downstream).

A `^fat` fn-value parameter that the CPS classifier threads (E2a, via the
registry; E2 via a fat closure's `fn_cps` slot), called in one arm of an `if`
or `match` that is a `let` init:

```turmeric
(defeffect Ask [] : int)
(defn fold-odd [acc : int i : int ^fat f : (fn [int int] int)] : int
  (if (> i 0)
    (let [nv (if (= (mod i 2) 1) (f acc i) acc)]
      (fold-odd nv (- i 1) f))
    acc))
(defn plus [a : int b : int] : int (+ a b))
(defn main [] : int
  (println (fold-odd 0 5 plus))                                        ; 9
  (println (handle (fold-odd 0 5 (fn [a : int b : int] : int
                                   (+ a (+ b (perform (Ask))))))
             (Ask [] k) (resume k 100)))                               ; 309
  0)
```

printed `5` and `105`: the first call's answer, returned as the loop's.

## How it was found

turmeric-spices `crdt` segfaulted in four tests (`test_counter`, `test_orset`,
`test_delta`, `test_converge`) once #1490 ("Read a module's members in the CPS
classifier's whole-program walks", `06b9d930`) landed. Bisected over the
`main` merges: `b3d1d4eb` passes, `06b9d930` crashes. `crdt/lattice`'s
`__mmw-loop` has the shape above (`nv (if has (f cur v) v)`); inside a
`defmodule` its `f` was never threaded until #1490 made module members
visible to the thread-parameter walk. At top level the same program was
already wrong. #1490 only made it reachable from a module. `__mmw-loop`
returned `max(cur, v)` in place of the merged map, and `gcounter-value`
crashed reading it as one.

## Root cause

The CPS IR gives the fn-value call a `CT_TAILCALL` whose continuation is the
local join the `let` body runs in (`KK_VAR`). The emitter's fn-value arms
(`src/compiler/emit_cps_ir.c`, the `via_registry` and `via_fncps` branches of
`CT_TAILCALL`) never looked at that continuation: they threaded `__kont`, the
function's own, so `f`'s answer was delivered to the caller.

The classifier is meant to keep this shape away from the emitter.
`needs_heap_join` evicts a function whose cps->cps tail call targets a join
it does not sit directly under. Its `CT_TAILCALL` case asked only
`tailcall_routes_cps_to_cps`, though `letcont_is_heap_join` and
`jbody_has_cps_tailcall` beside it count `via_registry` / `via_fncps` too.
Evicting would not have been right either: the direct path cannot thread an
effectful fn-value to its handler, so the second line above aborts with
"unhandled effect".

## Fix

- `join_escapes_lifted_rec`: a threaded fn-value tail call to a join, from
  anywhere but directly under its `CT_LETCONT` (which stays a heap join),
  makes the join *escaping*. `emit_escaping_join` then reifies it as a
  resume-frame.
- The fn-value arms of the `CT_TAILCALL` emitter thread that frame, or
  `__kont` from a lifted sub-frame, exactly as `emit_deliver_ty` delivers to
  an escaping join.
- `needs_heap_join` evicts a `KK_VAR` fn-value tail call only when its join is
  not one of the escaping joins in scope.

Pinned by `tests/fixtures/cps-threaded-fn-value-in-if-arm` (an `if` arm and a
`match` arm, each with a pure named function and an effectful lambda, inside
a `defmodule`). turmeric-spices `crdt`'s four tests pass again.

Writing the `match` variant turned up a separate defect, filed as
[cps-if-yielding-option-assigns-carrier-to-aggregate](../reported/cps-if-yielding-option-assigns-carrier-to-aggregate.md).
