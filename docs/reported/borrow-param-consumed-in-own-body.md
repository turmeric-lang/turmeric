# A `^borrow` parameter can be consumed inside the function that borrows it

**Severity:** medium (a silent hole in the linearity check: double close /
use-after-free compiles clean).

Found 2026-10-25 while adding `(fn [^borrow T] R)` callback types for
`spices/lmdb`'s scoped transactions.

## Repro

```turmeric
(defopaque H :int :linear)
(defn h-open [] : H ```c return 1; ```)
(defn h-close [h : H] : nil ```c (void)h; ```)
(defn sneaky [^borrow h : H] : int (do (h-close h) 0))   ;; consumes a borrow
(defn main [] : int
  (let [h (h-open)
        n (sneaky h)]
    (do (h-close h) (println n) 0)))                     ;; ...closed twice
```

`tur check` reports nothing. The same holds for a lambda,
`(fn [^borrow h : H] : int (h-close h))`, passed where a
`(fn [^borrow H] int)` is expected.

## Root cause

`^borrow` is enforced only at call sites: the caller's binding is not spent
(`elab_call.c`, the LB1 rollback after "Phase 11: Move tracking"). Inside the
callee the parameter binding is an ordinary linear binding, so passing it to a
consuming parameter is accepted like any other single use.

## Fix directions

Mark a `^borrow` parameter's binding as borrowed and refuse it in a consuming
position (an argument whose slot is neither `^borrow` nor copyable, a
`return`, a capture that escapes): TUR-E0101-shaped, "borrowed value 'h'
cannot be consumed here". Passing it on to another `^borrow` slot stays legal.
`spices/lmdb`'s `errors/` fixtures would gain a case once it lands.
