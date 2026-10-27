# Plan: Support direct application of anonymous lambdas

> **Status:** Draft Plan
> **Last Updated:** 2026-05-25
> **Type:** Compiler / Call elaboration / Codegen
> **Related:** [Fix spaced `: T` parameter type annotations](param-type-annotation-plan.md),
> [Haskell-Style Currying](upcoming/currying-plan.md)

---

## Overview

Turmeric currently rejects direct application of a captureless anonymous
lambda:

```turmeric
((fn [x] : int (+ x 1)) 3)
```

with:

```text
error: call head must be a symbol or closure expression
```

This is a separate existing limitation from the `fn` parameter-annotation
gap tracked in `param-type-annotation-plan.md`. Even after typed lambda
parameters are fixed, direct application still fails for a narrower reason:
the call elaborator only accepts non-symbol heads when they elaborate to a
fat closure (`TY_PTR_VOID`), while a captureless `fn` elaborates to a plain
function value (`EX_VAR` with `TY_FN`).

The user-facing symptom is "anonymous lambda application", but the real
language gap is broader:

- `((fn ...) arg)` should work
- any other non-symbol expression whose static type is callable should work
- existing captureful closure heads should keep working

This plan fixes the underlying "callable expression in head position"
handling rather than adding a one-off special case for anonymous lambdas.

---

## Current behavior

Confirmed against `build/tur` from `main`:

| Case | Example | Expected | Actual |
|---|---|---|---|
| Captureless anonymous lambda | `((fn [x] :int (+ x 1)) 3)` | `4` | `call head must be a symbol or closure expression` |
| Captureful anonymous lambda | `(let [y 2] ((fn [x] :int (+ x y)) 3))` | accepts | works today |
| Named binding of captureless lambda | `(let [f (fn [x] :int (+ x 1))] (f 3))` | accepts | works today |

So the gap is not "anonymous lambdas never work". It is specifically:

1. **Captureless** anonymous lambdas in direct call-head position.
2. More generally, any **non-symbol** head expression that elaborates to
   `TY_FN` instead of `TY_PTR_VOID`.

That means expressions like `((if cond f g) x)` and `((do f) x)` are very
likely part of the same limitation whenever the head expression's type is a
plain function type.

---

## Root cause

The mismatch is local and fairly crisp:

1. `elab_fn` in `src/compiler/elab_fns.c:1574-1585` returns:
   - `EX_VAR` of type `TY_FN` for **captureless** lambdas
   - `EX_CLOSURE` of type `TY_PTR_VOID` for **capturing** lambdas
2. `elab_call` in `src/compiler/elab_call.c:252-301` treats any
   non-symbol head as a special dynamic-call case, but only accepts it when
   `head_expr->type.kind == TY_PTR_VOID`.
3. Therefore `((fn ...) ...)` fails exactly when the `fn` is captureless,
   because its elaborated head expression is a function value, not a fat
   closure pointer.

There is also a structural smell in the current non-symbol-head branch:

- it is a CY1-era closure-call escape hatch, not a general callable-head path
- it hardcodes `TYPE_INT` for the synthetic `EX_CALL`
- it creates a temporary binding just so emit can name the head value
- it bypasses the normal "symbol call" path, so it is hard to extend cleanly

This is why the right fix is a proper callable-expression path, not
"accept `EX_VAR` here too" as an ad hoc patch.

---

## Non-goals

- **`fn` parameter annotations.** That remains tracked by
  `param-type-annotation-plan.md` TA2. This plan assumes `fn` syntax itself is
  valid; it does not solve the typed-params parser/elaborator gap.
- **New lambda syntax.** No shorthand syntax, no special "immediately invoked"
  form, no extra reader features.
- **Macro heads.** Macros and special forms remain symbol-headed; this plan is
  only about runtime-callable expressions.
- **Full closure ABI redesign.** Reusing the current `TY_FN` and fat-closure
  representations is preferred unless a narrow cleanup is needed for
  correctness.

---

## Design goal

Make head-position call elaboration answer one question:

> Does this expression elaborate to something callable?

If yes, elaborate the call using the expression's callable representation.
If no, emit a diagnostic that says the head is not callable.

The implementation should distinguish three cases:

| Head elaborates to | Meaning | Desired handling |
|---|---|---|
| `TY_FN` | plain function value / function pointer | ordinary indirect call |
| `TY_PTR_VOID` + closure provenance | fat closure | existing closure-call lowering |
| anything else | not callable | error |

That gets anonymous lambda application "for free" as one instance of the
general rule.

---

## Options

### Option A -- Minimal anonymous-lambda special case

Teach `elab_call` to recognize `head->tag == F_LIST` whose first symbol is
`fn`, then special-case `((fn ...) args...)`.

**Pros:** Smallest patch.

**Cons:** Wrong abstraction boundary. Does not help `((if cond f g) x)`,
`((do f) x)`, or any future feature that produces a callable head
expression. Hard-codes surface syntax into call elaboration.

### Option B -- Accept any non-symbol `TY_FN` head

Keep the current `TY_PTR_VOID` closure path, but extend the non-symbol-head
branch so `head_expr->type.kind == TY_FN` is also callable.

**Pros:** Fixes the actual bug with relatively small compiler churn.

**Cons:** Risks growing two unrelated call paths:

- `TY_FN` heads go through one lowering
- `TY_PTR_VOID` heads keep the old synthetic-temp hack

That would work, but it preserves duplication and diagnostic drift.

### Option C -- Unify callable-expression head handling

Factor a shared helper that classifies a callee expression and lowers calls
for:

- symbol-headed direct calls
- non-symbol `TY_FN` heads
- non-symbol fat-closure heads

Prefer this option.

**Pros:** One place for arity checks, result typing, currying interaction,
and diagnostics. Makes future callable-head forms easier.

**Cons:** Slightly larger refactor up front.

---

## Proposed approach

Take **Option C**.

### 1. Introduce a callee-classification helper

Add a helper in `elab_call.c` that, given an already elaborated head
expression, answers:

- callable kind: direct function vs fat closure
- full function type, if known
- whether the head must be evaluated once and stored in a temporary

This prevents the current "inspect `type.kind` inline and guess" pattern from
spreading.

### 2. Support non-symbol `TY_FN` heads directly

When the head expression has type `TY_FN`, build an `EX_CALL` that carries
the head expression through the existing indirect-call emit path rather than
rejecting it.

Important constraint: **evaluate the head exactly once.** If the head is a
compound expression (`if`, `do`, nested call, etc.), the elaborator should
wrap it in an `EX_LET` temporary before calling it so side effects are not
duplicated.

### 3. Keep fat-closure heads working, but move them behind the same helper

The existing `TY_PTR_VOID` path for captureful lambdas should remain valid,
but its lowering should stop being an inlined special branch in `elab_call`.

In particular, avoid preserving the current hardcoded `TYPE_INT` result on
the synthetic call node if a better result type can be recovered from the
closure expression's provenance (`EX_CLOSURE`, closure metadata, or a known
binding type).

### 4. Improve the diagnostic

If a non-symbol head expression elaborates successfully but is not callable,
say so explicitly:

```text
error: expression in call head has type `int`, which is not callable
```

That is more actionable than the current
"symbol or closure expression" wording once callable heads are supported.

---

## Interaction with existing plans

### `param-type-annotation-plan.md`

That plan should keep treating direct anonymous-lambda application as
separate from typed lambda parameters.

After this plan lands:

- TA2 fixes `(fn [x :T] ...)`
- this plan fixes `((fn ...) arg)`

Both are needed for `((fn [x :int] (* x x)) 3)` to work end-to-end.

### `currying-plan.md`

Currying already made under-saturated and over-applied calls more flexible,
but it did **not** solve callable-expression heads. This plan should reuse
the same arity reasoning as ordinary calls so currying behavior remains
consistent:

- under-saturated non-symbol `TY_FN` heads should partial-apply if ordinary
  symbol calls would
- over-application should keep chaining through callable results

That is another reason to unify, not fork, the call logic.

---

## Phases

### LA0 -- Repro fixtures and scope lock

Add fixtures that pin down current and desired behavior:

- `tests/fixtures/lambda-call-head/basic/`
  - `((fn [x] :int (+ x 1)) 3)` -> `4`
- `tests/fixtures/lambda-call-head/capturing/`
  - `(let [y 2] ((fn [x] :int (+ x y)) 3))` -> regression guard
- `tests/fixtures/lambda-call-head/if-head/`
  - `((if true f g) 3)` where both branches are callable
- `tests/fixtures/errors/noncallable-head/`
  - `((if true 1 2) 3)` -> "not callable" diagnostic

If any "should already work" case currently passes, record it as a passing
regression guard rather than rewriting it.

### LA1 -- Factor callee classification out of `elab_call`

Refactor `src/compiler/elab_call.c` so non-symbol-head handling goes through a
named helper rather than the current inline `if (head->tag != F_SYM)` branch.

Acceptance:

- no behavior change yet, except mechanical cleanup
- existing closure-head cases still pass

### LA2 -- Enable direct calls on `TY_FN` head expressions

Implement the actual language change:

- accept non-symbol heads whose elaborated type is `TY_FN`
- preserve single evaluation with `EX_LET` when needed
- lower to the same indirect-call machinery ordinary function-value calls use

Acceptance:

- `lambda-call-head/basic` passes
- named and captureful call paths keep passing

### LA3 -- Unify diagnostics and return-type handling

Tighten the remaining rough edges from the legacy fat-closure path:

- remove "symbol or closure expression" wording for callable-type failures
- infer or carry the correct call result type instead of defaulting to
  `TYPE_INT` in the non-symbol-head branch
- confirm emit handles both direct `TY_FN` and fat-closure heads without
  duplicate lowering code

Acceptance:

- `noncallable-head` fixture gets the new diagnostic
- no type regressions in existing closure/currying tests

### LA4 -- Docs and cross-links

- add a short note to user-facing docs if anonymous lambda examples are shown
- update `param-type-annotation-plan.md` to link here from the "separate
  existing limitation" note

---

## Test plan

Primary tests:

- `tests/fixtures/lambda-call-head/basic/`
- `tests/fixtures/lambda-call-head/capturing/`
- `tests/fixtures/lambda-call-head/if-head/`
- `tests/fixtures/errors/noncallable-head/`

Regression areas to rerun:

- currying fixtures (`tests/fixtures/currying/*`)
- closure fixtures
- async/effect fixtures that accept `EX_FN` / `EX_CLOSURE`

Manual smoke:

```turmeric
((fn [x] : int (+ x 1)) 3)

(let [y 2]
  ((fn [x] : int (+ x y)) 3))

(let [f (fn [x] : int (+ x 10))]
  ((if true f f) 1))
```

---

## Risks

- **Duplicating call semantics.** If `TY_FN` heads get their own bespoke path,
  currying, over-application, and diagnostics may diverge from ordinary calls.
- **Double evaluation of head expressions.** `((if (side-effect) f g) x)` must
  evaluate the head expression once.
- **Leaking the fat-closure hack further.** Reusing the current `TYPE_INT`
  synthetic-call shortcut would fix the immediate bug but make future callable
  expression work harder.
- **Over-broad acceptance.** Supporting callable expressions must not weaken the
  special-form / macro requirement that those remain symbol-headed.

---

## Exit criteria

This plan is done when all of the following are true:

1. `((fn [x] :int (+ x 1)) 3)` compiles and runs.
2. Existing captureful anonymous-lambda call heads still work.
3. At least one non-lambda callable head expression such as `((if cond f g) x)`
   also works.
4. Non-callable heads produce a type-based diagnostic, not the old
   "symbol or closure expression" message.
5. The implementation route is shared enough with ordinary calls that currying
   and future call elaboration changes only need to be updated in one place.
