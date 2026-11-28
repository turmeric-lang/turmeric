# A closure handed to a struct-building parameter is never freed

**Severity: low-medium (leak, one closure env per closure, 24 B in the repro).**
A caller's let-bound closure leaks when the callee stores it in a by-value
struct, even when that struct never leaves the callee.

Found 2026-11-28 while fixing
[fnfield-struct-binder-frees-borrowed-closure](../archive/fnfield-struct-binder-frees-borrowed-closure.md).
Before that fix the callee's struct binder freed the closure. That was wrong:
the second call ran a freed closure. Now nobody frees it.

## Repro

```turmeric
(defdata Ev (Start int) (End))
(defstruct H [raw : int handler : (fn [Ev] int)])
(defn mk [f : (fn [Ev] int)] : int
  (let [h (H 1 f)]
    ((.handler h) (End))))
(defn main [] : int
  (let [n 3
        g (fn [e : Ev] : int n)]
    (println (mk g))
    (println (mk g)))
  0)
```

Under `tests/run-leak-check.sh`'s build (ASan, `TUR_RUNTIME=source`):

```
SUMMARY: AddressSanitizer: 24 byte(s) leaked in 1 allocation(s).
```

Without the struct (`(defn mk [f : (fn [Ev] int)] : int (f (End)))`), `main`
frees `g` at scope end, and the program is leak-clean.

`tests/fixtures/fnfield-struct-binder-borrowed-closure` carries this repro and
its relatives (an unbound receiver `(.handler (H 1 f))`, a function that returns
the wrapped parameter) with a `known-leak` marker.

## Root cause

`main` frees `g` only when `g` does not escape (`let_binding_env_freeable`,
`src/compiler/emit_expr.c`). Passing `g` to `mk` escapes unless `mk`'s parameter
is non-retaining (`Binding.nonretain_param_mask`, inferred by
`elab_infer_nonretain_masks`, `src/compiler/elab_fns.c`, from
`closure_binding_escapes`). In `mk`, `f` is an argument of the `H` constructor.
The escape walk (`src/compiler/emit_core.c`) counts that as an escape, without
looking at where the constructed value goes.

## Fix directions

1. **Let the escape walk follow a constructor into a confined local.** `f` as
   a constructor argument does not escape when the constructed struct is bound
   by a `let` whose local is only read: fields read and called, never stored,
   returned, captured or passed on (`fnfield_local_leaves_scope` already
   answers the "returned" half). The same holds for the unbound receiver
   `(.handler (H 1 f))`.
2. Then the parameter is non-retaining, and the caller's existing free covers
   it. Delete the fixture's `known-leak` marker once it runs clean.
