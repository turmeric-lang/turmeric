# An effectful function passes into a `#fx{}` slot through a `let`

**Severity:** medium (an effect-typing hole). It also blocks narrowing the CPS
indirect-call rule (perf audit item R-1).

## Repro

```turmeric
(defn noisy [x : int] : int (println "side effect") x)
(defn pure-hof [g : (fn [int] #fx{} int) x : int] : int (g x))
(defn main [] : int
  (let [h noisy]                ; or  h : (fn [int] int)
    (println (pure-hof h 1)))
  0)
```

- `(pure-hof noisy 1)` is rejected: `TUR-E0009 ... argument's row {IO} is not
  a subset of parameter's declared row {}`.
- `(pure-hof h 1)`, with `h` bound to `noisy` by a plain or an annotated `let`,
  compiles, runs and prints `side effect`.
- With a handler effect instead of `IO`, the laundered call compiles past the
  checker and fails later, in CPS emission ("this effect operation has no
  lowering here"). That is a compiler-limitation error about a program the
  checker should have rejected.

A lambda literal is checked (`TUR-E0009`).

## Root cause (not traced to a line)

The row-subtype check at a call argument fires when the argument is a direct
reference to a function whose row is known: a named `defn` or a lambda. A
`let` binding's type carries no row of its own (an annotated
`(fn [int] int)` has none either), and an argument whose type has no row is
accepted into a slot with an explicit row instead of being treated as
"unknown".

## Fix directions

- Carry the effect row of a `let`'s initializer onto the binding, at least for
  an alias of a global function (`source_binding` already records it) and for a
  lambda initializer.
- Decide what a row-less function type means when it flows into a slot with a
  row. "Unknown" is the sound answer, which means rejecting it or requiring an
  annotation.
- This is item 1 of what R-1's fix needs
  ([wp2-runtime-hot-paths](../artifacts/perf-audit-2026-10/wp2-runtime-hot-paths.md),
  "What R-1's fix needs"). Until it is closed, a `#fx{}` row on a function-typed
  parameter cannot be used to skip CPS colouring: today such a parameter is
  coloured like any other (found by refined-fn-types-optimization-scan-plan,
  candidate 1).

## Resolution (2026-10-29)

Took the first fix direction. The ER4 call-site walk
(`check_call_site_rows_in_expr`, `src/passes/effect_check.c`) notes the
initializer of every immutable `let` binding it passes, when that
initializer is a variable or a lambda (`let_fn_init_note`). An argument that
names such a binding is followed back through the chain
(`let_fn_init_resolve`, bounded) to the named function or lambda, and that
row is checked. A plain alias, an annotated `h : (fn [int] int)`, an alias of
an alias, and a let-bound lambda that performs `IO` are all `TUR-E0009` now.
A `^mut` binding is not followed: it may hold something else by the time of
the call.

The second direction, deciding what a row-less function type means when it
flows into a slot with a row, is not done. It has no live repro today: a
row-less `(fn [int] int)` PARAMETER cannot be passed to a rowed slot at all
(`expected (fn [int] : int), got ptr<void>`), which is the open
[fn-param-not-forwardable-to-effect-rowed-param](../reported/fn-param-not-forwardable-to-effect-rowed-param.md).
Whoever fixes that one must decide this question there, or the hole opens
through a parameter. Until then R-1 (skipping CPS colouring for a `#fx{}`
parameter) is not safe on this fix alone.

Pinned by `errors/effect-row-let-alias-into-pure-param`,
`errors/effect-row-annotated-let-alias-into-pure-param` and
`errors/effect-row-let-lambda-into-pure-param`.
