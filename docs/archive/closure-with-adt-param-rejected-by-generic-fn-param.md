# A capturing closure whose parameter is a `defdata` type is refused by a generic fn-typed parameter

> **RESOLVED 2026-11-05.** The capturing path of `elab_fn`
> (`src/compiler/elab_fns.c`) built the closure VALUE's type with `type_fn` from
> the parameters' kinds alone; the env-shifted thunk type kept
> `arg_full_types`, the value's did not, so a `defdata` parameter read back as a
> def-less `<adt>`. The value's type now carries the full parameter types (the
> thunk's, past the env slot). Pinned by `closure-adt-param-generic-fn`, which
> runs the capturing and captureless lambdas side by side on all three engines.

**Severity: low-medium (a valid program is rejected; a hand-written recursion
works around it).** Found 2026-10-29 executing stats-formula-plan
(turmeric-spices `spices/stats-formula`).

## Repro

```turmeric
(defmodule rp/lam
  (export main)
  (defdata ^heap ^copy Box (Bx int))
  (defn fold [A B] [xs : (Cons A) init : B f : (fn [B A] B)] : B
    (if (tnil? xs) init (fold (ttail xs) (f init (thead xs)) f)))
  (defn main [] : int
    (let [xs (tcons-of (Bx 1) (tcons-of (Bx 2) (tnil)))
          k  10]
      (println (fold xs 0 (fn [a : int b : Box] : int (match b (Bx v) (+ a (+ k v)))))))
    0))
```

```
error [TUR-E0001]: function 'fold' arg 3: expected (fn [int Box] : int), got (fn [int <adt>] : int)
```

Reproduces on `main` (1ecdacc9) and v0.72.0, with `^heap` or without, and
with `fold` defined in the same module or imported. It does **not** reproduce
when the lambda captures nothing (drop `k`), when a named `defn` is passed
instead, or when `Box` is a `defstruct`. So the closure-conversion path
types the captured lambda's `defdata` parameter as a def-less `<adt>` (the
same rendering `pinned-instance-dispatch-loses-an-opaque-return-type` had),
which then fails to unify with `A := Box`.

## Root cause

Not traced. The `<adt>` says a type was rebuilt from its kind alone, without
`as.adt_.def` -- look where a lambda's parameter types are re-derived when it
is lifted with an environment.

## Fix directions

- Carry the parameter's full type (not its TypeKind) through closure lifting.
- Pin it with the repro above as a fixture (capturing and non-capturing
  variants side by side).

## Workaround

Recurse by hand instead of passing the closure to a generic helper
(spices/stats-formula/src/formula/factors.tur does).
