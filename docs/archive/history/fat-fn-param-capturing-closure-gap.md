---
title: Capturing Closure Cannot Reach a fn-typed ^fat Parameter
category: Reported Bug
description: A ^fat parameter cannot both accept a capturing closure value and be invoked directly. Bare ^fat g accepts a :ptr<void> capturing closure but is not directly callable; annotating it ^fat g :(fn [...] ...) makes it directly callable but then rejects any capturing closure because :ptr<void> does not unify with the fn type. Only captureless lambda literals (auto-shimmed) thread through the annotated form.
---

# Capturing Closure Cannot Reach a fn-typed ^fat Parameter -- Reported Bug

> **Status:** RESOLVED 2026-06-03. Cases A and B were fixed first; case C (a
>   bare `^fat g` with no fn-type annotation is now directly callable, defaulting
>   to a `:ptr<void>` fat box and dispatching through the fat protocol) and the
>   plain raw-`:ptr<void>` direct call (now a hard error -- raw pointer is not
>   callable, CRU B-4) landed via the closure-representation-unification /
>   closure-first-class-type work. Verified against the current build: case C
>   (`(run-with (make-adder 10) 5)` through a bare `^fat g` param) prints `15`.
>   The remaining bare-`^fat` *non-int-result* nuance is tracked (and now
>   diagnosed) in
>   [bare-fat-param-non-int-result-miscompiles.md](bare-fat-param-non-int-result-miscompiles.md).
> **Found:** 2026-06-03, during the Typed Closure Invocation ABI work
> **Severity:** Medium -- a real expressiveness hole in the ^fat parameter
>   surface; no silent miscompile (every broken combination is a hard
>   type/codegen error), but it blocks the obvious use case.
> **Related:**
> - [closure-typed-invocation-abi-plan.md](../upcoming/closure-typed-invocation-abi-plan.md)
> - [poly-to-fat-typed-shim-plan.md](../upcoming/poly-to-fat-typed-shim-plan.md)

---

## Summary

The `^fat` parameter marker lets a function consume a fat closure
(`{ thunk, env... }`) through the fat-call protocol. There are two ways
to write such a parameter, and **neither** lets you both (a) pass a
capturing closure value and (b) invoke it directly with `(g x)`:

| Parameter form | Direct call `(g x)` | Accepts a capturing closure value (`:ptr<void>`) |
|---|---|---|
| `^fat g` (no fn-type annotation) | **error**: `'g' is not a function or continuation` | yes |
| `^fat g :(fn [...] #{} :T)` (annotated) | works (since the fat-dispatch fix) | **error**: `:ptr<void>` does not unify with the fn type |

The only value that threads cleanly through the annotated form is a
**captureless lambda literal**, which the elaborator auto-shims into a
fat closure at the call site (`EX_FN_TO_FAT`). A closure that captures --
the whole reason fat closures exist -- cannot be passed by value to a
fn-typed `^fat` parameter, whether written inline or bound to a variable.

## Repro

All four cases below were run against the in-tree Debug build at the time
of reporting.

### (A) annotated form rejects an inline capturing lambda

```turmeric
(defn run-with [^fat g : (fn [:int] #{} : int) x : int] : int (g x))
(defn main [] : int
  (let [n 10]
    (println (run-with (fn [x : int] : int (+ x n)) 5)))   ;; captures n
  0)
```
```
error: ... (rejected at the (fn [x :int] :int (+ x n)) argument)
```

### (B) annotated form rejects a capturing closure via a variable

```turmeric
(defn run-with [^fat g : (fn [:int] #{} : int) x : int] : int (g x))
(defn make-adder [n : int] : ptr<void> (fn [x : int] : int (+ x n)))
(defn main [] : int
  (let [f (make-adder 10)]
    (println (run-with f 5)))    ;; f : :ptr<void> -- does not unify with :(fn ...)
  0)
```
```
error: ... (rejected at the f argument)
```

### (C) bare form accepts the closure but is not callable

```turmeric
(defn run-with [^fat g x : int] : int (g x))   ;; g : :ptr<void>
(defn make-adder [n : int] : ptr<void> (fn [x : int] : int (+ x n)))
(defn main [] : int
  (let [f (make-adder 10)]
    (println (run-with f 5)))
  0)
```
```
error: 'g' is not a function or continuation
   1 | (defn run-with [^fat g x :int] :int (g x))
     |                                     ^^^^^
```

### (D) what actually works -- captureless lambda + annotated form

```turmeric
(defn run-with [^fat g : (fn [:int] #{} : int) x : int] : int (g x))
(defn main [] : int
  (println (run-with (fn [x : int] : int (+ x 1)) 5))   ;; => 6
  0)
```

## Root cause

Two independent facts collide:

1. **A fat closure value has type `:ptr<void>`.** `make-adder` returns
   `:ptr<void>`; a let-bound or returned closure is a fat handle, not a
   `TY_FN`. The type checker treats `:ptr<void>` and `:(fn [...] :T)` as
   distinct and does not unify them, so any `:ptr<void>` argument is
   rejected at a fn-typed parameter (cases A and B). The auto-shim that
   rescues a captureless lambda (`EX_FN_TO_FAT`, created in
   `elab_call.c` when the argument's kind is `TY_FN`) only fires for a
   bare `(fn ...)` literal whose own type is `TY_FN`; a capturing
   closure's argument kind is already `TY_PTR_VOID`, so the shim branch
   is skipped.

2. **A bare `^fat g` parameter is `TY_PTR_VOID`, and `(g x)` on a
   `TY_PTR_VOID` *binding that is not a known closure* is rejected** by
   the callee-resolution path with `'g' is not a function or
   continuation` (case C). The bare form is only usable by handing `g`
   onward to an inline-C helper that does the fat dispatch by hand (the
   pattern in `tests/fixtures/captureless-autobox`'s `apply-fat`).

The recently landed fat-dispatch fix (direct `(g x)` on a fn-typed `^fat`
param now dispatches through slot 0) closed the *annotated*-form call
path, which is why (D) works. It did not -- and could not at the emit
layer -- address the *type-checker* refusal in (A)/(B) or the
callee-resolution refusal in (C).

## Impact

- The natural code -- "take a closure parameter and call it" -- is only
  expressible when the closure is captureless. Any real closure (one that
  closes over state) must be passed through a bare `^fat` parameter and
  invoked via hand-written inline-C, defeating the point of the typed
  fat-call surface.
- Combinator libraries (schema, signal arrows, parser combinators) that
  want to accept a user closure and apply it currently lean on
  `:ptr<void>` parameters plus inline-C `TUR_APPLY*`, which is exactly
  the erasure the Typed Closure Invocation ABI work is trying to retire.

## Status update (2026-06-03)

Cases **A** and **B** are fixed: a fn-typed `^fat` parameter now accepts a
capturing closure value (`:ptr<void>`) -- `elab_call.c` allows a
`TY_PTR_VOID` argument at a `TY_FN` parameter when that parameter is
`^fat` -- and the earlier fat-dispatch direct-call fix makes `(g x)`
correct. Verified for `:int` and `:float` by
`tests/fixtures/fat-param-capturing-closure`.

Case **C** (bare `^fat g` with no fn-type annotation is not directly
callable) and the plain `:ptr<void>` direct-call hazard
([ptr-void-direct-call-representation-split.md](ptr-void-direct-call-representation-split.md))
remain open and are folded into
[closure-representation-unification-plan.md](../upcoming/closure-representation-unification-plan.md)
(Phase 0 blockers).

## Proposed directions

Not yet scoped into an implementation plan; candidate approaches, roughly
in order of preference:

1. **Unify `:ptr<void>` fat-closure values with a fn-typed `^fat`
   parameter at the call site.** When a `^fat` parameter carries a
   `:(fn [A...] :R)` annotation and the argument is a `:ptr<void>`
   closure value, accept it (the value is already in fat-box form -- no
   shim needed) and let the call site dispatch through the typed-thunk
   cast. This makes (B) work and is the smallest conceptual change: the
   annotation describes how to *call* the box, and a fat box is exactly
   what is callable that way. Risk: distinguishing a genuine fat-closure
   `:ptr<void>` from an arbitrary `:ptr<void>` handle (may need an
   elaboration-time provenance bit or a dedicated closure type).

2. **Make a bare `^fat g` directly callable** by routing `(g x)` on a
   `^fat`-marked `TY_PTR_VOID` binding through the fat-dispatch emit path
   (the same path the annotated form now uses). This makes (C) work. The
   open question is the result/argument types: without the annotation the
   call site must infer them, which is why the annotated form was the
   tractable one to fix first.

3. **Carry closure signatures in a first-class closure type** instead of
   erasing capturing closures to `:ptr<void>`, so a closure value unifies
   with the matching `:(fn ...)` type directly. Largest change; subsumes
   (1) and (2) but touches the whole closure representation.

## Validation when fixed

- Cases (A), (B), and (C) above each compile and print the expected
  value.
- A capturing closure passed to a fn-typed `^fat` parameter and invoked
  directly round-trips for `:float` (register-class-distinct) as well as
  `:int`/`:ptr`/`:cstr`.
- `tests/fixtures/fat-param-direct-call` (captureless path) keeps
  passing; new fixtures cover the capturing path.
