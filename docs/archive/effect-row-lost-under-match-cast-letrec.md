# A `perform` under a `match`, an `as` or a `letrec` is missing from the inferred effect row

**RESOLVED 2026-10-09** (filed the same day). See "Fixed" at the end.

**Severity was: medium.** The effect pass inferred too small a row for any
function whose `perform` sat under one of about fifty node kinds. Three
things followed:

- A handler around a call to such a function warned TUR-W0033 ("handler
  clause for 'Ask' is unreachable") on a clause that runs, in both
  `tur build` and `tur --interpret`.
- A fn value of such a function looked pure to everything that asks its
  row. The poly-wrap that fills a fat value's `fn_cps` slot (`emit_expr.c`,
  EX_POLY_WRAP) is one, so a call through an un-annotated parameter took the
  direct path from a fresh root.
- The TUR-E0009 declared-row checks under those nodes never ran: the
  closure-row walk and the call-site row walk stopped at the same kinds.

Found 2026-10-09 while executing
[cps-effectful-callback-through-multi-arg-or-untyped-param](../reported/cps-effectful-callback-through-multi-arg-or-untyped-param.md):
`(defn ff [x : int] : float (+ 0.25 (as float (+ x (perform (Ask))))))`
inferred `#{}`.

## Repro

```turmeric
(defeffect Ask [] :int)
(defn m1 [o : (Option int)] : int (match o (Some v) (+ v (perform (Ask))) (None) 0))
(defn c1 [] : float (as float (perform (Ask))))
(defn main [] : int
  (println (handle (m1 (some 1)) (Ask [] k) (resume k 10)))
  (println (handle (c1) (Ask [] k) (resume k 10)))
  0)
```

`tur --dump-effects check` printed `defn m1 : #{}` and `defn c1 : #{}`, and
both handlers drew TUR-W0033. `tur --interpret` prints 11 and 10.

## Root cause

`collect_effects_in_expr` (`src/passes/effect_check.c`) is a switch with an
arm per node kind, and its `default:` returned the row unchanged. Every kind
without an arm read as "performs nothing below here". That included
`EX_MATCH`, which never had an arm, as well as `EX_CAST`, `EX_LETREC`,
`EX_SET_FIELD`, `EX_CONS_LIST`, the existentials, the STM and generator
nodes and more.

This is the same hole the CPS coloring walks had until
[effect-row-lost-through-a-constructor-argument](effect-row-lost-through-a-constructor-argument.md)
gave them `cps_visit_children` (`src/passes/cps.c`), a shared enumeration of
every kind's evaluated operands. The TUR-W0033 walk in this file already used
it. The row walk and the two checking walks (`check_closures_in_expr`,
`check_call_site_rows_in_expr`) did not.

A second, smaller gap was a call through a LOCAL name. A `let`-bound lambda,
a `let` alias of a named function and a captureless `letrec` member all
resolved to no FnDef, so their effects were not charged either.

## Fixed

- The `default:` arm of all three walks descends through
  `cps_visit_children`. Adding an arm is now a choice about non-uniform
  treatment, not a requirement for the subtree to be seen at all. Nested fn
  definitions are not enumerated, and EX_CLOSURE keeps its own arm.
- The EX_CALL arm follows a local name to the function it holds:
  `widen_fn_alias` (an immutable alias of a global fn or a lifted captureless
  lambda), then `source_binding` (a captureless `letrec` member), and for a
  local closure the lifted body's row through `closure_fn_binding`. The last
  merges the row only: the lifted entry's parameter list leads with its env,
  so it does not line up with the call's arguments for the parameter-row
  logic.

Measured: the whole fixture suite is unchanged (3642 passed with the walk
fix alone, no snapshot moved). No TUR-E0009 surfaced in the stdlib or the
corpus.

Pinned by `tests/fixtures/effect-row-under-match-and-cast`, which uses
`unexpected.stderr: TUR-W0033`. It has a user-ADT match, an Option match, an
`as` cast, a literal match, and a fn value whose only `perform` is under a
match, passed through an un-annotated parameter. The let-alias and `letrec`
shapes are still refused by the CPS backend, so they cannot be fixtures yet.
That is
[cps-local-fn-alias-or-lambda-called-in-place-refused](../reported/cps-local-fn-alias-or-lambda-called-in-place-refused.md),
and its repro no longer warns.
