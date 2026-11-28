# A let-bound closure passed to a closure factory is never freed

**Severity: low (leak, 24 B per call).** A caller binds a capturing closure
and passes it to a function whose result holds it: a lambda capturing the
parameter, or (since the fn-value join work) a box of the parameter, such as
`(if c f a2)`. The caller frees the factory's result after the call. Nothing
frees the closure it passed, although nothing reaches it any more.

Found 2026-11-28 while leak-checking
[fn-value-through-if-or-match-loses-its-representation](../archive/fn-value-through-if-or-match-loses-its-representation.md).
It reproduces on `main` at `5aebfd41` with the same numbers. There, the join
variants below also leaked their own boxes; on this branch only `cl` is left.

## Repro

```turmeric
(defn mk [f : (fn [int] int)] : (fn [int] int) (fn [x : int] : int (f x)))
(defn turn [k : int] : int
  (let [cl (fn [x : int] : int (+ x k))]
    ((mk cl) 10)))
(defn run [i : int n : int acc : int] : int
  (if (= i n) acc (run (+ i 1) n (+ acc (turn i)))))
(defn main [] : int (println (run 0 50 0)) 0)
```

It prints `1725`. Under ASan with `detect_leaks=1` it reports
`1200 byte(s) leaked in 50 allocation(s)`: `cl`'s env, allocated in `turn`.
`mk`'s own result is freed: `mk` is `returns_fresh_closure`, and the call-head
temp drops it. The same happens for a function that returns a join of its
parameter, `(defn sel [c f] : (fn [int] int) (if c f a2))`, and for one that
returns `(let [h (if c f a2)] h)`.

## Root cause

`cl`'s let frees it only when `closure_binding_escapes` says it stays in the
let (`let_binding_env_freeable`, `src/compiler/emit_expr.c`). Passing it to
`mk` reads as an escape. `mk`'s parameter escapes into its result, so its
`nonretain_param_mask` bit is clear, and that is correct: the result does hold
`cl`.

The fact the caller would need is "the result dies before `cl` does". The
compiler models that for exactly one shape: a fresh closure LITERAL at a
captured-parameter position is hoisted with `closure_dies_with_fn`
(`hoist_fresh_closure`, `src/compiler/elab_call.c`). The escape walk then
admits passing it to that callee (`emit_core.c`, the
`fresh_closure_captured_params` check), and both are freed after the call. A
let-bound closure has no such path.

## Fix directions

- Extend the `closure_dies_with_fn` relaxation to a let-bound closure. Every
  use must be an argument at a `fresh_closure_captured_params` position of a
  `returns_fresh_closure` call, and that call's result must itself be confined
  to the let: a call head, or a local that is only invoked. The local then
  dies with the last such result.
- Pin it in a `requires.leak-check` fixture with the must-keep twin: the
  factory's result returned or stored, called after the caller's let ends.
