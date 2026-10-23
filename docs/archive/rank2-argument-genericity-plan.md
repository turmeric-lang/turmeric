# Plan: check that a rank-2 (`forall`) argument is generic

> **Status:** DONE 2026-10-22 (from type-forms-gaps item 1). See
> [Outcome](#outcome) for what landed and what is still unchecked.

## Problem

```turmeric
(defn both [f (forall [a] (-> a a)) x : int s : cstr] : int
  (do (println (f s)) (f x)))
(defn inc [x : int] : int (+ x 1))
(defn main [] : int (println (both inc 5 "hi")) 0)
```

Accepted. `tur run` prints `i` (the string's address plus one, read as text) and
6; the interpreter prints a large integer and 6. A `forall [a]` parameter
promises the callee may instantiate `a` at any type at each use (hrt-guide); the
caller's obligation to pass something that works at every type is not checked.
Only a non-function (TUR-E0001) and a capturing closure are rejected
(`elab_call.c`, the HRT1/HRT4 rank-2 argument arm).

## The check

At a rank-2 argument whose callee is a named global function (`poly_arg_fn_binding`):
for each parameter of the forall body typed by a variable the forall binds
(`arg_full_types[i]->kind == TY_TYVAR`, name in `var_names`), the passed
function's parameter must be generic in that position -- a type variable, `any`,
or an un-annotated parameter (`Binding.type_is_carrier_default`, read through
`source_fn_def->params`). Otherwise TUR-E0001: `rank-2 argument 'inc' is not
generic: its parameter 1 has type int, but the parameter this is passed to
promises 'a' for every type`.

A prototype of exactly this was written and reverted on 2026-10-21 because the
corpus relies on the current behaviour (below). Notes from it:

- The forall body's `arg_kinds` are carrier kinds (`int`), so the bound-variable
  positions must be read from `arg_full_types`, which keep `TY_TYVAR` + name.
- The result type is not checked in the first slice: an un-annotated result
  reads as `int` with no "defaulted" marker on the function type. Add one, or
  check results only where the passed function's result is annotated.
- Higher-kinded foralls are already validated separately
  (`hrt_validate_hk_actual`); skip them here.

## The opt-out

Keep an explicit ascription as the trusted override: `(:: succ (forall [a] (->
a a)))` asserts the genericity, the way `hrt-rank2-annotation` uses it. The check
runs only on a bare named function, not on an `EX_ASCRIBE` to a `TY_FORALL`.

## Fixture migration

The reverted prototype rejected these (the list was cut at ten, so run the whole
suite for the rest): `currying-rank2-partial`, `await-through-rank2-param`,
`hrt-forall-constraint-multi`, `hrt-forall-constraint-show`, `hrt-impred-closure`,
`hrt-impred-let`, `hrt-impred-reuse`, `hrt-integration`,
`hrt-poly-call-cstr-result`, `hrt-rank2-annotation`. Each passes an `int`-only
function and applies it only at `int`. Migrate each to a generic function
(`(defn id [A] [x : A] : A x)`, or a constrained one for the `Show` fixtures)
unless the fixture is about the ascription opt-out, which keeps its ascription.
Regenerate their snapshots in the same change.

## Steps

1. The check plus the opt-out, behind nothing (it is a soundness fix, not a
   feature; CLAUDE.md's experiment rule does not apply to diagnostic
   strictness).
2. Migrate the fixtures; add `errors/rank2-argument-not-generic` (the program
   above) and a passing fixture for the ascription opt-out.
3. hrt-guide and the `forall` entry of `stdlib/forms/types.tur`: replace "not
   checked" with the rule and the opt-out; regenerate the docstrings.

## Outcome

Landed as planned, with three widenings:

- **Every rank-2 argument site**, not only the HRT1/HRT4 call arm:
  `hrt_check_rank2_arg_generic` (`src/compiler/elab_call.c`) also runs at a
  partial application (CY4), a rank-3 argument, and a typeclass method's rank-2
  parameter (`elab_typeclasses.c`, receiver and argument positions).
- **Forward references are checked, not trusted.** A function defined below the
  call has no `FnDef` yet, and its pass-1 declaration cannot tell `[x]` from
  `[x : int]`, so the check is parked on the binding (`Elab.rank2_pending`) and
  run by `hrt_rank2_pending_flush` when the definition's `FnDef` is built.
- **Lambdas are checked** like named functions -- they lift to one -- and the
  diagnostic names them `(a lambda)` rather than `__fn_N`.

Fixtures: 22 rejected. Identity-shaped functions became generic
(`currying-rank2-partial`, `fn-type-constraint`, `hrt-impred-let`,
`hrt-rankn-propagation`, `hrt-rank2-identity`, `hrt-rankn-rank3`,
`hrt-rankn-typeclass`, `hrt-poly-call-cstr-result`), the `Show` ones
constrained generics (`hrt-forall-constraint-show`; `hrt-forall-constraint-multi`
now shows both arguments, `42`). Functions that are int-only by nature -- `inc`,
`double32`, the awaiting `level3` family -- keep their behaviour through the
ascription opt-out (`emit-abi-trace`, `hrt-impred-closure`, `hrt-impred-reuse`,
`hrt-integration`, `hrt-rank2-apply`, `hrt-rankn-hkt`, `hrt-stdlib-church`,
`hrt-stdlib-cont`, `phase-f-poly-concrete`, `phase-f-poly-concrete-unsigned`,
`await-through-rank2-param`, `await-below-unthreaded-frame-refused`), as does
`benchmarks/bench-poly-specialize.tur`. None carry an `expected.c`.

New: `errors/rank2-argument-not-generic` (the program above) and its
`-forward`, `-lambda` and `-method` siblings; `rank2-argument-generic` for the
accepted shapes and the opt-out (`hrt-rank2-annotation` already exercised it).

Still unchecked: the result type; a bound variable under a type application
(`(List a)`); a callee with no `FnDef` at all (an `extern-c`); a local
function value (`!is_global`) reaching a forall slot. Found on the way: passing
an `any`-parameter function to a forall slot type-checks but segfaults at run
time, before and after this change --
[rank2-any-param-function-segfaults](rank2-any-param-function-segfaults.md).
