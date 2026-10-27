---
title: Bare ^fat Parameter Silently Miscompiles a Non-int Closure Result
category: Reported Bug
description: A directly-called bare ^fat parameter (no fn-type annotation) is :ptr<void>, and the :ptr<void> direct-call path hard-codes the result type to int64_t. Calling a closure that returns :float (or another non-int-register-class type) through a bare ^fat param reads the wrong register and yields garbage -- a silent miscompile. The annotated form (^fat g :(fn [...] :T)) threads the correct result type and works.
---

# Bare ^fat Parameter Silently Miscompiles a Non-int Closure Result -- Reported Bug

> **Status:** RESOLVED (2026-06-03) via fix direction 1 (diagnose). The silent
>   miscompile is gone: a bare `^fat g` call in the **result position** of a
>   `:float` function -- where the int64-typed call would be returned through the
>   wrong register (rax vs xmm0) -- is now a hard error directing the user to the
>   annotated form `^fat g :(fn [...] :float)`, which threads the correct result
>   type and prints `7`. The diagnostic (`elab_fns.c`, `bare_fat_int_tail_call`
>   + the check in `elab_defn`) only fires when the bare-`^fat` call is the
>   tail/result expression of a `:float` function, so legitimate non-tail uses
>   (e.g. `(int->float (g x))`, or a genuinely int-returning closure) are not
>   rejected. `:cstr`/`:ptr` returns share the integer register and continue to
>   round-trip, so they are intentionally not diagnosed. Bare `^fat` int and the
>   annotated form are unchanged. Regression fixture:
>   `tests/fixtures/errors/bare-fat-float-result`. Directions 2 (infer the result
>   type from context) and 3 (first-class closure type) remain the end-states for
>   making the *bare* form work for `:float` without annotation; until then the
>   annotated form is the supported non-int path. `bash tests/run.sh`: 0 FAIL.
> **Found:** 2026-06-03, Phase 0 of the closure-representation work
>   (blocker 3).
> **Severity:** Medium -- silent wrong value (no crash, no diagnostic) for
>   a non-int closure result through a bare `^fat` param. Narrow: requires
>   using bare `^fat` (rather than the typed form) with a non-int closure.

---

## Summary

A bare `^fat g` parameter (no fn-type annotation) is typed `:ptr<void>`
(closure-representation-unification Phase 0, blocker 1, made it directly
callable). The `:ptr<void>` direct-call elaboration hard-codes the call's
result type to `int64_t` (`elab_call.c`, the `TY_PTR_VOID` branch builds
`EX_CALL` with `TYPE_INT`). When the closure actually returns `:float`
(xmm0) the int64 cast reads the integer register (rax) instead -- a
register-class mismatch -- so the value is garbage. No diagnostic fires;
the wrong value flows on.

## Repro

```turmeric
(defn run-with [^fat g x : float] : float (g x))         ;; bare ^fat
(defn make-scale [k : float] : ptr<void> (fn [x : float] : float (* x k)))
(defn main [] : int
  (println (run-with (make-scale 2.0) 3.5))            ;; expect 7.0
  0)
```
Prints `4.61957e+18` (garbage), not `7`.

The **annotated** form is correct:

```turmeric
(defn run-with [^fat g : (fn [:float] #{} : float) x : float] : float (g x))
;; (run-with (make-scale 2.0) 3.5) => 7
```

A fn-typed `^fat` param is `TY_FN`, so the call goes through the ER2
`is_fat` path which uses the call's `e->type` (the declared `:float`
result) -- the result type is correct there.

## Root cause

A bare `^fat g` has no declared signature, so the `:ptr<void>` direct-call
path has no result type to thread and falls back to `int64_t`. The result
type is genuinely unknown for a bare `^fat` closure value (the closure's
true signature was erased to `:ptr<void>` at the parameter boundary).

## Implications and guidance

- Bare `^fat` is appropriate for **int-carrier** generic closures (the
  arrow / `>>>` / map-style combinators, which are int64 at runtime). The
  int result is correct there.
- For a closure returning `:float` or any non-int-register-class type, use
  the **typed** form `^fat g :(fn [argtypes] #{} :RetType)`, which threads
  the correct result type.

## Proposed fix directions

1. **Diagnose instead of miscompiling.** When a bare `^fat` (`:ptr<void>`)
   call's `int64_t` result is used in a non-int context (e.g. returned
   where `:float` is declared), emit a hard error directing the user to
   annotate the parameter with a fn type, rather than silently coercing
   the int bits. Removes the silent miscompile cheaply.
2. **Infer the result type bidirectionally** at the `:ptr<void>` call from
   the expected type of the call's context (the enclosing return type or
   binding), when available. More work; recovers the non-int case without
   an annotation.
3. **First-class closure type** (the unification plan's end-state) so a
   closure value never erases its signature to bare `:ptr<void>`.

## Validation when fixed

- The bare-`^fat` `:float` repro either prints `7` (directions 2/3) or
  fails to compile with a clear "annotate the ^fat parameter" message
  (direction 1) -- never silently prints garbage.
- The annotated form keeps printing `7`.
