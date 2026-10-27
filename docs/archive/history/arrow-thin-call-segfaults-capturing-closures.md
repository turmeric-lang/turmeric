---
title: stdlib/arrow.tur Thin-call Helpers Segfault on Capturing Closures
category: Reported Bug
description: __arrow_call1 / __arrow_call2 and the __arrow_pair_* helpers invoke their function argument as a bare int64_t(*)(int64_t...) pointer with no environment, which is correct only for captureless arrows. Composing capturing closures through >>> / arrow-first / arrow-second / par-comp / arrow-split passes a fat-closure box where a bare function pointer is expected and segfaults. The stdlib-arrow fixture only exercises captureless arrows, so the crash is latent.
---

# stdlib/arrow.tur Thin-call Helpers Segfault on Capturing Closures -- Reported Bug

> **Status:** RESOLVED (2026-06-03) in closure-representation-unification
>   Phase 1. Every arrow combinator now takes `^fat` closure parameters and
>   dispatches through the fat protocol: `arr`, `>>>`, `arrow-first`,
>   `arrow-second`, `par-comp`, and `arrow-split` are `^fat`, and the
>   `__arrow_pair_*` helpers invoke their (already-fat) function arguments via
>   `TUR_APPLY1` rather than the old bare `int64_t(*)(int64_t)` thin cast. A
>   captureless function auto-shims into a fat box at the `^fat` call site; a
>   capturing closure is already a fat box -- both flow through the same path.
>   The `__arrow_call1/__arrow_call2` thin helpers are gone. Regression fixture:
>   `tests/fixtures/arrow-capturing-closure` composes *capturing* closures
>   through every combinator (the latent segfault pre-fix); `stdlib-arrow` keeps
>   the captureless coverage. `bash tests/run.sh`: 0 FAIL.
> **Found:** 2026-06-03, confirming a risk flagged in the Typed Closure
>   Invocation ABI plan ("Thin-vs-fat cast discrepancy in current
>   stdlib... Today's code works for reasons not fully understood").
> **Severity:** High -- a hard **segfault**, not a warning, on ordinary
>   use (composing closures that capture). Masked only because the test
>   suite exercises captureless arrows exclusively.
> **Related:**
> - [closure-typed-invocation-abi-plan.md](../upcoming/closure-typed-invocation-abi-plan.md) (Risks: thin-vs-fat cast discrepancy)
> - [stdlib-arrow-scaleback-plan.md](../upcoming/stdlib-arrow-scaleback-plan.md) -- follow-up that scaled `stdlib/arrow.tur` back to its bare-function combinators after this fix landed, removing the dead Arrow typeclass declarations that had no working instances.
> - [closure-returning-instance-method-codegen-plan.md](../upcoming/closure-returning-instance-method-codegen-plan.md) -- the *separate* codegen bug that blocked giving those Arrow declarations real instances (a closure-returning `definstance` method's dict-field/impl/return carrier erased instead of carrying the `int64_t` closure handle). Fixed 2026-06-03; the Arrow reintroduction is now unblocked.

---

## Summary

Every function-applying helper in `stdlib/arrow.tur` invokes its callable
argument as a **bare** function pointer with no environment slot:

```turmeric
(defn __arrow_call1 [f x] : int
  ```c return ((int64_t(*)(int64_t))(intptr_t)f)(x); ```)
```

That cast -- `int64_t (*)(int64_t)` -- is only correct when `f` is a
**captureless** function (a bare C function pointer). A closure that
captures free variables is a **fat-closure box** `{ thunk, env... }`; its
real thunk has signature `int64_t (*)(void *env, int64_t)`. Calling the
box address as `int64_t(int64_t)` jumps to the box's first word (the
thunk pointer value) as if it were code, and passes the argument where
the environment pointer belongs. The result is a segfault.

The same flaw is in `__arrow_call2` and all the `__arrow_pair_*` helpers
(`__arrow_pair_first`, `__arrow_pair_second`, `__arrow_pair_par`,
`__arrow_pair_split`), which back the public combinators `>>>`,
`arrow-first`, `arrow-second`, `par-comp`, and `arrow-split`.

## Repro

Minimal reproduction of the exact cast `__arrow_call1` uses, applied to a
capturing closure (`make-add` closes over `n`):

```turmeric
(defn thin-call [f x] : int                       ;; == __arrow_call1
  ```c return ((int64_t(*)(int64_t))(intptr_t)f)(x); ```)
(defn fat-call [f x] :int                        ;; correct fat dispatch
  ```c return TUR_APPLY1(f, x); ```)
(defn make-add [n : int] : ptr<void> (fn [x : int] : int (+ x n)))
(defn main [] : int
  (let [f (make-add 100)]
    (println (fat-call f 5))     ;; => 105
    (println (thin-call f 5)))   ;; => Segmentation fault
  0)
```

Output:
```
105
Segmentation fault
```

`fat-call` (using `TUR_APPLY1`, which reads the thunk from slot 0 and
passes the box as the env) returns the correct `105`; the
`__arrow_call1`-style `thin-call` crashes.

The public surface crashes the same way -- e.g. `(>>> a b)` where `a` or
`b` is a capturing closure routes through `__arrow_call1 fv` with `fv`
holding a fat box.

## Why the test suite is green

`tests/fixtures/stdlib-arrow` exercises arrows built from captureless
function literals only (no captured state), so `f` is always a bare
function pointer and the thin cast happens to be correct. Nothing covers
a capturing arrow, which is the common case for real signal-processing
pipelines (a gain stage closing over its coefficient, a stateful filter,
etc.).

## Root cause and the representation split

The deeper issue is that an arrow value has **two incompatible runtime
representations** and the helpers assume one of them:

- a captureless `(fn ...)` is a bare `int64_t (*)(int64_t)` pointer;
- a capturing `(fn ...)` is a fat-closure box `{ thunk, env... }`.

`__arrow_call1`'s thin cast is right for the first and wrong for the
second. Symmetrically, naively switching it to `TUR_APPLY1` (fat
dispatch) would be right for the second and wrong for the first (a bare
pointer has no slot-0 thunk to read). A correct fix has to make the
representation uniform at the boundary.

## Breadth update (2026-06-03)

The same thin-call defect is **not arrow-specific**. A grep for inline-C
that casts a closure parameter to a bare `int64_t (*)(...)` pointer (no
slot-0 read) found the identical hazard in:

- `stdlib/option.tur` `option-map` (`((int64_t(*)(int64_t))(intptr_t)f)(...)`),
- `stdlib/comonad.tur`, `stdlib/pair.tur`, `stdlib/mutmap.tur`.

All crash on a capturing closure for the same reason. This is a systemic
stdlib pattern, so the fix is tracked centrally in
[closure-representation-unification-plan.md](../upcoming/closure-representation-unification-plan.md)
(Phase 1) rather than per file.

Note also `contract.tur:65` passes a handler to a C function as a genuine
raw function pointer (`(void (*)(const char *))(intptr_t)handler`) -- that
one must stay bare, which is why blanket boxing is not a valid fix (see the
plan's "proven dead ends").

## Proposed fix directions

1. **Normalise arrows to fat closures at construction.** Make `arr`
   (and the combinators that accept raw functions) take a `^fat`
   parameter so a captureless function is auto-shimmed into a fat box
   (`EX_FN_TO_FAT`) and a capturing closure is already fat. Then the
   helpers dispatch uniformly via `TUR_APPLY1` / `TUR_APPLY2` (or the
   typed `TUR_APPLY*_T` once concrete element types are known). This is
   the smallest change that makes both cases correct and aligns arrow
   with the Typed Closure Invocation ABI direction.
2. **Type the arrow surface** so the compiler boxes/dispatches closures
   through the typed-thunk path instead of hand-written int64 casts,
   retiring `__arrow_call*` entirely (the parent plan's end state for the
   arrow combinators).

Note the interaction with
[fat-fn-param-capturing-closure-gap.md](fat-fn-param-capturing-closure-gap.md):
direction 1 needs a `^fat` parameter to accept a capturing closure value,
which is the very gap reported there. The two should be fixed together.

## Validation when fixed

- The repro above prints `105` then `105` (or the arrow result) with no
  crash.
- New `tests/fixtures` cases compose **capturing** arrows through `>>>`,
  `arrow-first`, `arrow-second`, `par-comp`, and `arrow-split` and check
  the results.
- Existing captureless `stdlib-arrow` coverage still passes.
- Runs clean under the Debug ASan build (the crash is currently a wild
  jump; ASan/UBSan should also flag it once a fixture exercises it).
