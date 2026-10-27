---
title: Intra-Instance Method Dispatch (`.method self`) Is Unsupported
category: Reported Bug
description: Calling one typeclass method from inside another method's body of the same instance -- via `(.other self ...)` -- fails elaboration with "no typeclass method found for '<other>'". Dispatch on the receiver works fine from outside the instance; only the in-instance-body case fails.
---

# Intra-Instance Method Dispatch (`.method self`) Is Unsupported -- Reported Bug

> **Status:** FIXED (found 2026-06-03, fixed 2026-06-04)
> **Severity:** Medium -- an **ergonomics / expressiveness gap** that surfaces
>   as a hard elaboration error, not a miscompile. It blocks the natural
>   "method B is defined in terms of method A" idiom, forcing a shared
>   top-level helper instead.
> **Found:** while executing
>   [closure-returning-instance-method-codegen-plan](../upcoming/closure-returning-instance-method-codegen-plan.md);
>   surfaced designing the T6.5 "via other method" coverage fixture.

## Summary

Inside a `definstance` method body, dispatching to another method of the **same
instance** on the receiver `self` -- `(.base self ...)` -- fails to resolve:

```
error: no typeclass method found for 'base'
```

The receiver is correctly typed as the instance type, and the exact same
dispatch works from any *other* function (e.g. from `main`). The failure is
specific to a `.method` call that appears while the enclosing `definstance` is
still being elaborated.

## Minimal repro

```turmeric
(defclass HasN [a]
  (base  [self n : int] : int)
  (twice [self n : int] : int))
(defstruct ArrW [a] (raw :int))
(definstance HasN [ArrW]
  (base  [self n : int] (+ n 1))
  (twice [self n : int] (.base self (* n 2))))   ;; <- error here
(defn main [] : int
  (let [w (:: (make-struct ArrW 0) (ArrW int))]
    (println (.base w 41)))   ;; this form is FINE from outside
  0)
```

```
repro.tur:7:24: error: no typeclass method found for 'base'
```

Note the receiver type does not matter (the non-closure `:int` return above
fails identically), so this is independent of the closure-return carrier work.

## Root-cause direction

Method-dispatch resolution (`.method`) looks the method up against the set of
registered typeclasses/instances. When the call occurs inside a method body of
an instance that is still mid-elaboration, the instance's methods are not yet
visible to the dispatcher, so the lookup for the sibling method fails.

Inspect the `.method` dispatch resolution path in `src/compiler/elab_call.c`
(method-call elaboration) and the point in
`src/compiler/elab_typeclasses.c` where instance method bodies are elaborated
(`elab_definstance`): the sibling methods should be resolvable while elaborating
each body -- either pre-register the instance's method bindings before
elaborating any body, or let `.method` fall back to the class's method table
(the concrete impl is selectable from `self`'s type).

## Validation

- The repro elaborates without error and prints `83` from `(.twice w 41)`
  (`((41*2)+1)`).
- A fixture where one closure-returning method builds its closure by calling a
  sibling method on `self` compiles and runs.

## Fix

`elab_definstance` (`src/compiler/elab_typeclasses.c`) used to register the
instance into `e->typeclass_env.instances` only *after* the loop that
elaborates every method body had finished, so a `(.sibling self ...)` call in
one body could not see the instance and the dispatcher in `elab_method_call`
reported "no typeclass method found".

The registration was hoisted to *before* the body-elaboration loop: the
instance is registered and its `type_args` / `method_impls` slots wired up
(all impls initially `NULL`), then each `method_impls[i]` is filled in as that
method's body is elaborated. A method dispatching to an earlier sibling now
sees a populated slot, and the instance itself is already on the env list so
the type-based dispatch selects it.

This resolves the natural "method B is defined in terms of method A" idiom,
including a closure-returning method that builds its closure by calling a
sibling on `self`.

Validation: `tests/fixtures/instance-intra-method-dispatch` exercises both the
simple `(.base self ...)` case (prints `83`) and the closure-returning
"via sibling" case (prints `105`).

### Forward references and mutual recursion (now fixed, 2026-06-04)

The original fix filled each `method_impls[i]` slot *as that method's body was
elaborated*, so a method dispatching to a sibling defined **later** in the same
`definstance` (or two mutually recursive methods) still failed -- the callee's
slot was still `NULL` when the caller's body was elaborated.

This is now resolved by splitting `elab_definstance`'s body loop into two
passes (the "two-pass split" the original report proposed):

1. **Pass 1** parses every method's signature (name / params / return type) and
   creates its `FnDef` + binding shell, populating `method_impls[i]` -- but
   leaves the body as a nil placeholder. After pass 1 *every* sibling slot is
   non-`NULL`, with the param bindings and fn-type the dispatcher needs.
2. **Pass 2** elaborates each method body against the now-complete instance, so
   a call to a sibling defined later -- or a mutually recursive pair -- resolves
   because the dispatcher finds a populated slot.

The method's file-scope registration (`elab_register_file_def` + global
`scope_add`) is deliberately deferred to pass 2, *after* the body is
elaborated, preserving the original `[body lambdas...][method def]` emit order.
Registering the method def in pass 1 would emit the method ahead of its
closure's lifted thunk; the closure's file-scope env struct (written while
emitting the method body) would then land textually inside the method, out of
scope for the later thunk -- an "undefined `struct __env_N`" miscompile.

Validation: `tests/fixtures/instance-intra-method-forward-ref` exercises a
forward `(.base self ...)` dispatch (prints `83`) and a mutually recursive
`is-even`/`is-odd` pair (prints `1` then `0`).

#### Latent bug surfaced and fixed alongside

The two-pass split shifts the global synthetic-id counter (`Elab.next_id`),
which perturbs arena allocation offsets. That exposed a pre-existing latent
defect: the union-match arm path in `elab_structs.c` (the `(match x (n : int)
...)` form on a `(int | bool)` scrutinee) allocated its `MatchArm` array
without initialising `arm.guard`. The arena is never zeroed, so `guard` held
arena garbage; readers such as `scan_adt_apps_in_expr` (the ADT
monomorphisation walk, run only for union/ADT programs) did
`if (arm.guard) recurse(arm.guard)` and dereferenced the garbage -- a
SEGV that previously "worked by luck" whenever that slot happened to be zero.
Fixed by initialising `arm.guard = NULL` in that path, matching the sibling
session-offer / literal / ADT match paths.
