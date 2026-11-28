# A function returning a captureless lambda through a `let` or `do` leaks a box per call

**Severity: low (leak, 24 B per call).** A function whose declared result is a
`(fn ...)` returns a lambda with no captures from the tail of a `let` or a
`do`. Each call mallocs a `{ shim, fn }` box that nobody frees. Returned
directly, the same lambda is freed by the caller.

Found 2026-11-28 while leak-checking
[fn-value-through-if-or-match-loses-its-representation](../archive/fn-value-through-if-or-match-loses-its-representation.md).
It reproduces on `main` at `5aebfd41` with the same numbers.
`tests/fixtures/let-tail-noncapturing-lambda-fat-temp` has both shapes:
`wrap-callable` and `via-do`.

## Repro

```turmeric
(defn via-do [] : (fn [int] int) (do 1 (fn [c : int] : int (+ c 200))))
(defn run [i : int n : int acc : int] : int
  (if (= i n) acc (run (+ i 1) n (+ acc ((via-do) i)))))
(defn main [] : int (println (run 0 50 0)) 0)
```

It prints `11225`. Under ASan with `detect_leaks=1` it reports
`1200 byte(s) leaked in 50 allocation(s)`: the box allocated in `via-do`.
`(let [_x 1] (fn [c : int] : int (+ c 100)))` as the body leaks the same.

## Root cause

Stage 2 of fn-value fat normalization (`elab_normalize_fn_tail_leaves`,
`src/compiler/elab_fns.c`) shims the tail leaf into a heap box
(EX_FN_TO_FAT, without `static_ok`). It marks the declared result `boxed`,
but leaves the `let` / `do` wrapper typed as it was.

The caller frees a returned box only for a `returns_fresh_closure` callee.
`fn_join_returns_fresh_box` infers that for a body that is the box itself, or
a join typed `boxed`. A `let` or `do` wrapper with a thin type is neither, so
it declines. A bare `(fn ...)` body, `direct` in that fixture, is the box
itself, which is why it is freed.

## Fix directions

- Give stage 2's shim of a bare defn or lifted lambda the shared static box
  (`static_ok`, plus `opaque_static` for the drop-glue warning). It is a
  link-time constant, so nothing is allocated, and a caller's drop of it is
  the no-op keep glue.
- Or have stage 2 re-type the wrappers it passes through, as
  `fn_join_box_leaves` does, so `fn_join_returns_fresh_box` sees the result as
  boxed.
