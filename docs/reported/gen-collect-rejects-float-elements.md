# `gen-collect` of a float generator is a type error

**Severity: low.** Found 2026-10-23 executing
[generator-type-plan](../archive/generator-type-plan.md). Reproduces on `main`
before that work.

## Repro

```turmeric
(load "stdlib/gen.tur")
(defn main [] : int
  (let [arr (gen-collect (gen [] (yield 1.5) (yield 2.5)))]
    (println (gen-arr-len arr)))
  0)
```

```
stdlib/gen.tur:182:37: error [TUR-E0001]: function 'gen-arr-push!' arg 2: expected int, got float
```

## Root cause

`gen-unwrap` is typed at the element (generator-yield-payload-is-int64-only),
so a float element reaches `gen-arr-push! [a val]`, whose unannotated `val`
defaults to `int`. The `gen-arr` helpers store raw 64-bit words and were never
typed; the error points into the macro expansion rather than the call.

## Fix directions

Type the gen array (`(Vec T)` built by the macro), or have `gen-collect` store
the slot's bits (`(:: ... :int)` erasure, read back by a typed `gen-arr-get`).
The first is the one that does not repeat the `:int` stand-in.
