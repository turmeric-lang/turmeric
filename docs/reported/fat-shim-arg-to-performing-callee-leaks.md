# A defn passed to a `^fat` parameter of a function that performs leaks its shim

**Severity: low (leak, 24 B per call).** If a function performs an effect, a
named defn passed to one of its `^fat` parameters leaks the `{ shim, fn }` box
the caller builds for the call. Nothing about the callee's use of the
parameter matters: calling it and nothing else is enough.

Found 2026-11-28 while leak-checking
[carrier-fn-param-in-join-evicts-effectful-fn](../archive/carrier-fn-param-in-join-evicts-effectful-fn.md).
It reproduces on `main` at `5aebfd41`, so the join has nothing to do with it.

## Repro

```turmeric
(defmodule m
  (defeffect Ask [] : int)
  (defn a1 [x : int] : int (+ x 1))
  (defn ff [^fat f : (fn [int] int)] : int (+ (perform (Ask)) (f 10)))
  (defn main [] : int
    (println (handle (ff a1) (Ask [] r) (resume r 1000)))
    0))
```

It prints `1011`. Under ASan with `detect_leaks=1` it reports
`24 byte(s) leaked in 1 allocation(s)`, allocated in `main__cps`:

```c
void *__t204 = malloc(sizeof(void *) + 2 * sizeof(int64_t));
...
return m__ff__cps((int64_t)(intptr_t)__t2, __h0); /* cps->cps */
```

The same caller with a callee that does not perform, such as
`(defn ff [^fat f : (fn [int] int)] : int (f 10))` called inside the `handle`,
reclaims the box.

## Root cause (likely, not traced to the line)

The caller's shim is stack-allocated, or handed to the DK entry reap inside a
CPS function, only when the sink is proven non-retaining: `stack_ok` on
EX_FN_TO_FAT (`elab_call.c`), driven by the callee's `nonretain_param_mask`.
That mask comes from `closure_binding_escapes` over the callee's body
(`elab_infer_nonretain_masks`, `elab_fns.c`). That walk treats an `EX_PERFORM`
conservatively as an escape. A `perform` anywhere in the body therefore clears
the bit for every fn parameter, and the shim is heap-allocated with no owner.

## Fix directions

- For a fn parameter's non-retention bit, use `closure_binding_escapes_reaped`
  (which treats a `perform` as not an escape) when the caller would reap the
  shim at its DK entry boundary rather than free it at scope exit. That is the
  same distinction `cps_closure_env_freeable` already draws.
- Pin it with a `requires.leak-check` fixture: a defn and a capturing closure,
  each passed to a `^fat` parameter of a performing callee.
