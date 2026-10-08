# TUR-W0033 calls a handler clause unreachable when the effect arrives through a function value

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

## Likely cause (not investigated)

The reachability check reads the handle body's inferred effect row. An
un-annotated `(fn [int] int)` parameter admits an effectful argument -- the
CPS coloring threads it, which is why the program works -- but the row the
check sees for `(use1 ask-plus 1)` does not carry the argument's `Ask`. The
check should either take the argument's row into account at the call, or
stay quiet when the body calls through a fn value whose row is open.

Not every call site warns: the same `(handle (loop-eff 0 100 0) (Ask [] k)
(resume k 10))` written inline in `main` of that fixture does not. Working out
why is part of the fix.
