# A closure in a `Some` a function returns is never dropped by its caller

**Severity: low (leak, 24 B per call).** A function returns
`(Some <capturing lambda>)`, and the caller matches the result in place and
only calls the closure. The closure's env is never freed.

Found 2026-11-28 while leak-checking
[fn-value-through-if-or-match-loses-its-representation](../archive/fn-value-through-if-or-match-loses-its-representation.md).
It reproduces on `main` at `5aebfd41` with the same numbers.
[sum-closure-payload-never-dropped](sum-closure-payload-never-dropped.md)
covers a `Some` held in a local, and lists "return the Option" only as a
shape the callee must keep. This is the other side: the caller that receives
the Option.

## Repro

```turmeric
(defn mk-some [k : int] : (Option (fn [int] int)) (Some (fn [x : int] : int (+ x k))))
(defn turn [k : int] : int
  (match (mk-some k) (Some h) (h 10) (None) 0))
(defn run [i : int n : int acc : int] : int
  (if (= i n) acc (run (+ i 1) n (+ acc (turn i)))))
(defn main [] : int (println (run 0 50 0)) 0)
```

It prints `1725`. Under ASan with `detect_leaks=1` it reports
`1200 byte(s) leaked in 50 allocation(s)`: the closure env allocated in
`mk-some`. `(let [o (mk-some k)] (match o ...))` leaks the same way. On this
branch, a function returning a fn-value join in a `Some`,
`(Some (if c f a2))`, leaks the join's box the same way too.

## Root cause

The sum-payload drop (`let_binding_sum_closure_freeable`,
`src/compiler/emit_expr.c`) recognises a fresh payload in two ways: a
constructor over a closure written right there, or a call whose callee's whole
body is a constructor over its one parameter (`call_wraps_sole_arg_in_ctor`,
the stdlib `some` / `ok` / `err` shape). `mk-some` builds the closure itself,
so neither matches. A `match` on the call written in place has no let to hang
the drop on either. `match_scrut_owns_carrier_cell` frees a fresh carrier
cell there, but not a closure payload.

## Fix directions

- Infer "returns a sum whose payload is a fresh closure" for a function whose
  tail constructs one over a capturing lambda, as `returns_fresh_closure` is
  inferred for a bare lambda (`elab_fns.c`), under the same capture and result
  gates.
- Accept such a call in `let_binding_sum_closure_freeable`, and give a
  `match` on it, written in place, the live arm's drop when every arm only
  invokes its binder (`closure_binding_escapes`).
