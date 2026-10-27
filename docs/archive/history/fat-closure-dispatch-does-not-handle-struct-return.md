---
title: Fat-closure dispatch ABI does not handle aggregate (struct) returns
category: Bug Report
description: A closure declared to return a struct value (e.g. `(Pair float float)`) emits an inner body whose C-level return type is the struct, but the fat-dispatch site casts it to `int64_t (*)(...)`. The C compiler rejects the resulting code outright -- it is not a silent miscompile, but it is a hard build break that blocks any signal-style API that wants to return a typed Pair from a closure.
---

# Fat-closure dispatch ABI does not handle struct returns

## Summary

Severity: **hard error** (C compile failure on the generated code).

A function that returns a closure whose declared result type is a struct
(`(Pair A B)`) emits an inner closure body returning that struct directly,
and a dispatch shim that casts to `int64_t (*)(void*, ...)`. The two C
types are incompatible; clang rejects the assignment.

This was surfaced by spike G4 of
`docs/upcoming/language-readiness-for-typed-signal-plan.md`.

## Minimal repro

```turmeric
;; tests/fixtures/pair-signals-typed/input.tur (now removed; repro lives here)

(defn make-scaler [k : float] : ptr<void>
  (fn [x : float] : float (* x k)))

(defn pair-signals
  [^fat sa : (fn [:float] #{} : float)
   ^fat sb : (fn [:float] #{} : float)] : ptr<void>
  (let [av sa bv sb]
    (fn [t : float] : (Pair float float)
      (make-struct Pair (av t) (bv t)))))

(defn call-pair
  [^fat f : (fn [:float] #{} : (Pair float float)) t : float] : (Pair float float)
  (f t))

(defn main [] : int
  (let [s2 (make-scaler 2.0)
        s3 (make-scaler 3.0)
        p  (call-pair (pair-signals s2 s3) 1.0)]
    (println (pair-fst p))
    (println (pair-snd p)))
  0)
```

`tur emit-c` succeeds. `cc` on the result fails:

```
error: returning 'Pair__float__float' from a function with
       incompatible result type 'int64_t'
        return (Pair__float__float){.fst = ..., .snd = ...};
               ^
error: returning 'int64_t' from a function with incompatible result
       type 'Pair__float__float'
        return ((int64_t (*)(void*, double))(intptr_t)((int64_t *)f)[0])(f, t);
               ^
```

The closure body and the dispatcher disagree on the C return type.

## Root cause sketch

Looking at the generated C:

```c
/* inner body returns the struct directly */
static Pair__float__float __fn_870(void *env_p, double t) {
    /* ... */
    return (Pair__float__float){.fst = ..., .snd = ...};
}

/* call_pair dispatches as int64_t-returning */
static Pair__float__float call_pair(int64_t f, double t) {
    return ((int64_t (*)(void*, double))(intptr_t)((int64_t *)f)[0])(f, t);
    /*     ^ cast to int64-returning fn ptr; cannot then be returned as struct */
}
```

The fat-dispatch macro (`TUR_APPLY1` / its inline equivalents) was written
when every closure result fit in an int64-shaped register. Float results
got their own typed thunk family
(`tur_thunk_double_double_t`, etc., introduced for #200/#208), but
aggregate returns still fall through to the int64 family.

## Proposed fix directions

1. **Extend the typed-thunk family to aggregates.** When the closure's
   declared result type is a struct (or any non-scalar), the dispatcher
   should be the matching typed thunk, not the `int64_t`-returning generic
   one. This is the same pattern as the float-thunk fix; the work is
   plumbing the result-C-type information through to the call site.
2. **Box aggregate results.** If (1) is too invasive, return a pointer to
   a heap-allocated `Pair` instead. This pushes one allocation per call
   into the hot path, so it is the fallback rather than the preferred
   answer.
3. **Reject at elaboration.** If neither (1) nor (2) is on the near-term
   path, fail the closure-returning-aggregate construct at
   elaboration time -- a hard diagnostic is far better than a generated
   C file that the C compiler refuses.

## Validation

A fix is valid when the repro above compiles and prints `2\n3\n` under
both Debug and Release builds, leak-clean under ASan. The fixture
directory `tests/fixtures/pair-signals-typed/` is currently a stub; once
a fix lands, re-add the input.tur and `expected.stdout` of `2\n3\n`.

## Resolution

Fixed. The diagnosis in "Proposed fix directions" was slightly off: the
typed-thunk family (`use_typed_thunk_abi` / `ensure_typed_thunk_typedef`)
*already* accepts `TY_STRUCT` results -- it was simply never reached
because the aggregate result type was dropped to the `int64_t` carrier
before it got there. Three sites lost the type:

1. `src/compiler/elab_types.c` -- the `(fn [param-types] #{row} :ret)`
   fat-fn-type parser built the `TY_FN` with `result_kind` only and never
   stored `result_full_type`, so a call through a `^fat` parameter typed
   `:(fn [...] :(Pair float float))` saw the result as the bare carrier.
   Now preserves the full type for `TY_APP` / `TY_ADT` / concrete
   `TY_STRUCT` results (the `(-> ...)` arrow parser got the same fix).
2. `src/compiler/elab_fns.c` (`elab_fn`) -- the lambda return-annotation
   parser preserved `return_full_type` only for tyvar/`TY_FN` results, so
   the lifted closure `(fn [t] : (Pair float float) ...)` was typed with a
   def-less struct and emitted an `int64_t`-returning thunk over a
   struct-returning body. Now preserves it for `TY_APP` / `TY_ADT` /
   concrete `TY_STRUCT`.
3. `src/compiler/emit_expr.c` -- a separate latent bug surfaced by the
   repro: `let`-binding a call whose declared result is a concrete
   carrier-ABI aggregate (`(Pair float float)`, `(Vec int)`) declared the
   binding as `int64_t` even though `emit_fns.c` returns such functions'
   results *by value*. Added `call_returns_byvalue_aggregate` so the
   binding (and the by-value-bridging predicate) match what the callee
   actually returns. Generic (tyvar-carried) and inline-C results still
   use the `int64_t` carrier.

Validation: `tests/fixtures/pair-signals-typed/` re-added (the repro
above) with `expected.stdout` of `2\n3\n`; passes under `bash
tests/run.sh` (1378 fixtures, 0 fail) and runs leak-clean under
ASan/LSan in both Debug and Release.

## Cross-references

- Surfaced by [[language-readiness-for-typed-signal-plan]] gap G4.
- Related: the closure-typed-invocation-abi-plan and bare-fat-result-type
  -inference-plan handled scalar (float/cstr) return types but not
  aggregates.
