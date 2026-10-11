# A `defer` in a `let` body evicts the function from the CPS backend when the enclosing `do` also carries defers

**Severity: low-medium (a hard error, not a wrong answer).** Found 2026-10-11
while fixing [defer-skipped-on-panic-in-cps-function](../archive/defer-skipped-on-panic-in-cps-function.md);
it predates that change (reproduced on the base branch).

## Repro

```turmeric
(defn note [s : cstr] : int
  ```c printf("%s\n", s); fflush(stdout); return 0; ```)
(defeffect Ask [] : int)

(defn f [x : int] : int
  (defer (note "outer"))
  (let [a (+ (perform (Ask)) x)]
    (defer (note "inner"))
    a))

(defn main [] : int
  (handle (f 1) (Ask [] k) (resume k 1)))
```

```
error: this effect operation has no lowering here: the enclosing function
left the CPS backend's supported subset, and the direct emitter cannot lower
`perform`.
```

`TUR_TRACE_EVICT=1` reports `BODY-TAINT` for `f`. Drop the outer `(defer
(note "outer"))` and it compiles and runs (the inner defer fires at the
`let`'s exit); drop the inner one and it compiles too. Only the combination
evicts.

## Root cause (not pinned down)

The P5b `do` lowering in src/passes/cps_ir.c threads a block's defers into
its continuation when every defer body is `safe_to_delegate`. With defers in
both the outer `do` and the `let` body, something in the nested lowering is
no longer admitted -- `first_unsupported` finds nothing (the trace would say
`BODY-UNSUPPORTED`), the term's core shape passes, so it is the taint check
(`BODY-TAINT`). Start from the eviction trace in `emit_cps_ir_try_fn_impl`
(src/compiler/emit_cps_ir.c, the `cat` classification) and compare the two
single-defer variants' terms with the combined one.

## Fix directions

Find which admission check rejects the nested shape and widen it; both
single-defer shapes already lower, so the combined term should too.
