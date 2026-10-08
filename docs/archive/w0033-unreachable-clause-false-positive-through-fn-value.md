# TUR-W0033 calls a handler clause unreachable when the effect arrives through a function value

**RESOLVED 2026-10-08** (filed and fixed the same day). The cause was wider
than a diagnostic. An un-annotated `(fn [int] int)` in parameter position is
a rank-2 poly-fn param (`ptr<void>` at the C level, its signature on
`poly_type`, no row). The effect pass carried a fn-value argument's row only
into a row-VARIABLE param (ER2), so the argument's effects were charged to
nothing. That had two consequences:

- **The false warning:** `(handle (use1 ask-plus 1) (Ask ...))` read as
  performing nothing, so the clause was reported unreachable.
- **A run-time abort:** `(defn wrap [i] (use1 ask-plus i))` inferred `#{}`,
  so wrap's own fn value was built with no CPS entry, and
  `(handle (use1 wrap 2) (Ask [] k) (resume k 10))` aborted with
  `unhandled effect`.

`collect_effects_in_expr`'s call arm now charges such an argument's row to
the call when the matching param is fn-typed with no written row (`TY_FN`
with a NULL row, or a poly-fn param whose `poly_type` carries none). It peels
the ascription, fat-normalization and poly-fn wrappers first. This is
conservative for a callee that only stores the value: the effect still
happens when something calls it. `wrap` now infers `#{Ask}`.

The "not every call site warns" puzzle below was a separate false NEGATIVE.
The unreachable-clause walk had explicit arms and stopped at any other node,
so a `handle` written as `println`'s argument (an `EX_BUILTIN`) or inside a
match arm was never checked. Its default arm now descends through
`cps_visit_children`. A genuinely dead clause in `(println (handle ...))`
warns now.

Pinned by `tests/fixtures/effect-through-open-fn-param` (the shapes above,
including the formerly aborting `(use1 wrap 2)`, with an `unexpected.stderr`
of `TUR-W0033` -- a new `run.sh` expectation file, the mirror of
`expected.stderr`) and `tests/fixtures/effect-handle-unreachable-in-builtin-arg`
(the printed dead clause). No codegen snapshot moved; suite 3626/0, turi
2670/0, JIT over the effect/async/CPS fixtures 312/0, the shallow-handler
W0033 probes 6/0. The sibling `turmeric-spices` checkout was not present, so
a spice whose declared `#fx{...}` row was relying on the lost effects would
show up as a new TUR-E0009 there, not here.

**Severity: low-medium (misleading diagnostic).** The warning says the handle
body "does not perform" an effect that it does perform: the effect is
performed by a function passed as a value to a callee whose fn-typed
parameter has an un-annotated row. The clause runs, and the program is right.
But anyone who follows the warning and deletes the clause gets an
`unhandled effect` abort at run time.

Found 2026-10-08 while writing `tests/fixtures/fn-value-call-join-reclaimed`,
whose `twice-sum` triggers it.

## Repro

```turmeric
(defeffect Ask [] : int)
(defn ask-plus [x : int] : int (+ x (perform (Ask))))
(defn use1 [f : (fn [int] int) i : int] : int (f i))
(defn a [] : int (handle (use1 ask-plus 1) (Ask [] k) (resume k 10)))
(defn b [] : int (handle (use1 ask-plus 1) (Ask [] ^multishot k) (+ (resume k 1) (resume k 2))))
(defn main [] : int (println (a)) (println (b)) 0)
```

```
w.tur:4:55: warning [TUR-W0033]: handler clause for 'Ask' is unreachable: the body does not perform 'Ask'
w.tur:5:66: warning [TUR-W0033]: handler clause for 'Ask' is unreachable: the body does not perform 'Ask'
```

and then `tur run` prints `11` and `5`, the clauses' answers.

## Likely cause (as filed; see the top for the measured one)

The reachability check reads the handle body's inferred effect row. An
un-annotated `(fn [int] int)` parameter admits an effectful argument -- the
CPS coloring threads it, which is why the program works -- but the row the
check sees for `(use1 ask-plus 1)` does not carry the argument's `Ask`. The
check should either take the argument's row into account at the call, or
stay quiet when the body calls through a fn value whose row is open.

Not every call site warns: the same `(handle (loop-eff 0 100 0) (Ask [] k)
(resume k 10))` written inline in `main` of that fixture does not. Working out
why is part of the fix.
