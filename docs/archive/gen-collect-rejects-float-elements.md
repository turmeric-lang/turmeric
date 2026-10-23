# `gen-collect` of a float generator is a type error

**Resolved 2026-10-23.** `gen-collect` now builds a `(Vec T)` with `vec-new` / `vec-push!` (fix direction 1), typed at the generator's element, so float and `cstr` generators collect. That surfaced a second bug the macro needed fixed: the S4 forward element inference that lets a `(vec-push! v x)` pin `(vec-new)`'s element (`src/compiler/elab_call.c`) was gated on `macro_expand_depth == 0`, so a push written inside ANY macro call -- `gen-collect`'s loop, or a user's `(when c (vec-push! v 1.5))` -- left `v` at `(Vec int)` and stored the float's bits. The gate is gone (full `tests/run.sh` stays green). `gen-arr-*` stay for `seq-collect`, whose elements are carrier words. Pinned by `tests/fixtures/gen-collect` (int, float, cstr) and `tests/fixtures/vec-push-in-macro-pins-element`.

**Severity: low.** Found 2026-10-23 executing
[generator-type-plan](generator-type-plan.md). Reproduces on `main`
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
