---
title: Direct Call of a :ptr<void> Callback Crashes Half the Time (closure representation split)
category: Reported Bug
description: Calling a :ptr<void> parameter directly via (f ...) assumes one of two incompatible closure representations depending only on arity -- the nullary path emits a thin bare-pointer call, the n>0 path emits fat-closure dispatch. A captureless closure is a bare function pointer and a capturing closure is a fat box, so each arity works for one representation and segfaults on the other. This is the shared root cause behind the arrow thin-call crash and the fn-typed ^fat parameter gap.
---

# Direct Call of a :ptr<void> Callback Crashes Half the Time -- Reported Bug

> **Status:** RESOLVED (2026-06-03) by closure-representation-unification
>   Phase 3 / Option B (sub-phases B-1, B-2, B-4). The representation split is
>   gone: capturing closures are now a first-class boxed `TY_FN` value (B-1) that
>   shares `:ptr<void>`'s carrier; a fat-closure *sink* is the `^fat` parameter,
>   which fat-dispatches for ALL arities including `n == 0` (B-2) and auto-boxes
>   a captureless argument; and a *raw* `:ptr<void>` is no longer directly
>   callable -- doing so is a compile error (B-4), so `:ptr<void>` is
>   raw-pointer-only again. The four arity x capture combinations now all
>   dispatch correctly through the `^fat` sink (or are a typed error for a raw
>   pointer). Fixtures: `fat-param-nullary-closure`, `fat-param-capturing-closure`,
>   `stdlib-test-runner-callback` (migrated to `^fat`), and negative
>   `errors/ptr-void-raw-not-callable`. See
>   [closure-first-class-type-plan.md](../upcoming/closure-first-class-type-plan.md).
> **Found:** 2026-06-03, during the Typed Closure Invocation ABI work
> **Severity:** High -- silent **segfault** on two of the four
>   arity x capture combinations, with no diagnostic.
> **Related (same root cause):**
> - [arrow-thin-call-segfaults-capturing-closures.md](arrow-thin-call-segfaults-capturing-closures.md)
> - [fat-fn-param-capturing-closure-gap.md](fat-fn-param-capturing-closure-gap.md)

---

## Summary

When a `:ptr<void>` parameter is invoked directly with `(f ...)`, the
emitter (`emit_expr.c`, the `if (fn_binding->type.kind == TY_PTR_VOID)`
branch) chooses the dispatch shape **by arity alone**:

- **n == 0** (`emit_expr.c:1753`): emits a thin bare-pointer call
  `((R (*)(void))f)()` -- correct only if `f` is a bare function pointer.
- **n > 0** (`emit_expr.c:1763`): emits fat-closure dispatch (read the
  thunk from slot 0, pass the box as the env) -- correct only if `f` is a
  fat-closure box `{ thunk, env... }`.

But a closure's runtime representation is **not** determined by arity --
it is determined by whether it captures:

- a **captureless** `(fn ...)` is a bare `int64_t (*)(...)` function
  pointer (no slot 0);
- a **capturing** `(fn ...)` is a heap fat box `{ thunk, env... }`.

So each arity is correct for one representation and **segfaults** on the
other:

| | captureless (bare fn pointer) | capturing (fat box) |
|---|---|---|
| **n == 0** (thin call) | works | **segfault** (box address called as code) |
| **n > 0** (fat dispatch) | **segfault** (slot 0 of a bare pointer) | works |

## Repro

```turmeric
;; n == 0, capturing -> segfault
(defn invoke0 [f : ptr<void>] : int (f))
(defn make-k [n : int] : ptr<void> (fn [] : int n))      ;; captures n -> fat box

;; n > 0, captureless -> segfault
(defn invoke1 [f : ptr<void> x : int] : int (f x))
;; (invoke1 (fn [x :int] :int (+ x 1)) 5)  -- bare fn pointer, fat dispatch crashes

(defn main [] : int
  (println (invoke0 (fn [] : int 7)))        ;; captureless n0 -> 7  (works)
  (println (invoke1 (make-k 0)))             ;; (n/a; illustrative)
  0)
```

Observed individually:

- `invoke0` with a **captureless** `(fn [] :int 7)` -> prints `7`.
- `invoke0` with a **capturing** `(make-k 42)` -> **segfault**.
- `invoke1` with a **capturing** `(make-add 40)` -> prints `42`.
- `invoke1` with a **captureless** `(fn [x :int] :int (+ x 1))` ->
  **segfault**.

The emitted nullary call is literally:

```c
static int64_t invoke0(void * f) {
    return ((int64_t (*)(void))f)();   /* f may be a fat box -> jumps to box[0] as code */
}
```

## Why it cannot be fixed locally

The call site has only a `:ptr<void>` -- it cannot tell a bare function
pointer from a fat box, so neither dispatch is universally correct. The
existing code "works" for whatever representation each call happens to
hit in the test suite; it is not a correct general lowering. Forcing the
nullary path to fat dispatch would fix the capturing case and **regress**
the (currently working) captureless case, and vice versa. (This is why
the recent fn-typed `^fat` direct-call fix only applies under the `^fat`
marker: `^fat` guarantees the value has been normalised to a fat box via
`EX_FN_TO_FAT`, removing the ambiguity. The bare `:ptr<void>` path has no
such guarantee.)

## Root cause

This is the same closure-representation split that underlies the
[arrow thin-call crash](arrow-thin-call-segfaults-capturing-closures.md)
and the
[fn-typed ^fat parameter gap](fat-fn-param-capturing-closure-gap.md): a
closure value has two incompatible ABIs (bare pointer vs. fat box) and
the surrounding code guesses which one it has. Until closures have a
single, self-describing representation at a callable boundary, any code
that invokes a `:ptr<void>`/untyped closure must know statically which
form it holds.

## Investigation update (2026-06-03)

Two empirical findings constrain the fix:

- **Plain `:ptr<void>` direct call is an intended, pervasive fat-dispatch
  convention.** Making `(f x)` on a non-`^fat` `:ptr<void>` an error broke
  **2117** tests: stdlib relies on it (e.g. `stdlib/list.tur` `__cons-fmap`
  directly calls a plain `:ptr<void>` param). So the n>0 fat-dispatch shape
  is "correct by convention"; the bug is that captureless inputs are not
  boxed and the n==0 path is inconsistent.
- **Blanket boxing of captureless fns at `:ptr<void>` sinks is unsafe.**
  `contract.tur:65` passes a handler to a C function as a raw function
  pointer; boxing it would hand C a fat box instead of a function pointer.

The fix must therefore normalize only fat-dispatched closure sinks, not
raw C-callback sinks, and is tracked in
[closure-representation-unification-plan.md](../upcoming/closure-representation-unification-plan.md)
(Phases 2-3).

## Proposed fix directions

1. **Normalise to fat at the boundary.** Require closure-callback
   parameters to be `^fat` (or otherwise box captureless values at the
   call site, as `EX_FN_TO_FAT` already does for `^fat`), so every
   directly-callable closure value is a fat box and a single fat-dispatch
   lowering is always correct -- for all arities. This also fixes the
   arrow helpers and the `^fat` parameter gap.
2. **A self-describing closure value** (tagged or uniformly fat) so the
   representation no longer depends on capture. Larger change; subsumes
   (1).

Either way, the arity-based dispatch in the `TY_PTR_VOID` branch should
be deleted in favour of one representation-correct lowering.

## Validation when fixed

- All four cells of the matrix above compile and run without crashing.
- A `:float`-returning capturing closure invoked through the boundary
  round-trips (register-class-distinct check).
- Existing captureless callback fixtures still pass.
- Runs clean under the Debug ASan/UBSan build with a fixture that
  exercises each cell.
