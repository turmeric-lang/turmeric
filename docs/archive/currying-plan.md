# Haskell-Style Currying -- Implementation Plan (CY0–CY4)

> **Status:** CY0–CY4 complete.
>
> **Prerequisites:** Phase 2 (closures, `fn`, `defn`), Phase HRT1 (`->` type
> constructor), Phase HKT (kind system). No effect-row interaction is required
> for CY0–CY2; effect-row propagation through partial applications is deferred
> to CY4.
>
> **Last updated:** 2026-05-27

---

## Motivation

Today every Turmeric function must be called with exactly the number of
arguments it declares (`TUR-E0002` otherwise). This prevents:

- Point-free style: `(map inc xs)` instead of `(map (fn [x] :int (inc x)) xs)`
- Operator sections: `(+ 1)` as a unary increment
- Pipelines where intermediate functions accept partial arguments
- Standard functional patterns like `(fold (+ 0) xs)`

Haskell-style currying makes every N-argument function also a valid
`(N-1)`-argument function that returns a closure waiting for the remaining
argument. This is the same strategy used by Haskell (fully curried GHC Core),
OCaml (multi-arg workers with arity-checking at call sites), and Standard ML.

---

## Design Decisions

### Internal representation: worker/wrapper

Rather than converting every function to single-argument form (like GHC Core),
Turmeric uses the **worker/wrapper** model that OCaml and MLton use:

- The compiled function keeps its N-argument calling convention in C (the
  "worker"). No performance regression for fully-saturated calls.
- When a call site provides fewer arguments than the function expects, the
  elaborator synthesises a **partial-application closure** (the "wrapper") that
  captures the supplied arguments and exposes the remaining arity.

This means: no change to `FnDef`, no change to normal codegen, no ABI break.
Partial application is purely a call-site transformation emitted by the
elaborator.

### Type of a partial application

If `f : (-> A B C)` and we write `(f a)` where `a : A`, the result has type
`(-> B C)`. The elaborator constructs a fresh anonymous closure whose body is
`(f a b)` where `b` is a fresh parameter. The existing closure machinery in
`elab_fn` / `emit.c` handles the rest.

For a fully curried chain `(f a b)` where `f : (-> A B C)`, this is just the
normal fully-saturated call -- no wrapper is needed.

### Over-application

`(f a b c)` where `f : (-> A B)` and `(f a b) : C` and `C` is itself a
function type `(-> D E)` -- the result is called with `c`. The elaborator
handles this by elaborating `f` with as many args as it will accept, then
treating the result as the new callee for the remaining arguments, recursively.
This requires detecting that the intermediate result type is callable.

### No implicit uncurrying

`(fn [a b] :int (+ a b))` keeps its 2-argument worker. It is not automatically
converted to `(fn [a] :... (fn [b] :int (+ a b)))`. The only change is that
calling it with 1 argument now synthesises a wrapper instead of an error.

---

## Architecture Overview

```
src/elab.c          -- elab_call_fn: partial-application synthesis (CY1)
                      elab_form:    over-application chaining (CY2)
src/emit.c          -- no changes needed; uses existing closure emit path
src/types.h         -- no changes needed; partial-app type is TY_FN
tests/fixtures/     -- currying-*.tur fixtures (CY0–CY3)
stdlib/             -- update map/filter/fold/zip signatures (CY3)
```

The existing closure infrastructure (`EX_CLOSURE`, `Closure`, `FnDef` with env
parameter, `emit_closure`) is reused without modification. The partial-
application wrapper is just a synthetic anonymous function that the elaborator
inserts.

---

## Phase CY0 -- Specification and fixtures

**Goal:** Define the surface semantics with runnable examples before touching
the compiler. All fixtures in this phase are expected to *fail* until CY1 is
complete.

### Semantics spec (in-doc)

| Expression | `f` type | Result type | Behaviour |
|---|---|---|---|
| `(f a)` | `(-> A B)` | `B` | fully saturated, normal call |
| `(f a)` | `(-> A B C)` | `(-> B C)` | partial application, returns closure |
| `(f a b)` | `(-> A B C)` | `C` | fully saturated |
| `(f a b c)` | `(-> A B C)` where `C = (-> D E)` | `E` | over-application |
| `((f a) b)` | `(-> A B C)` | `C` | explicit two-step, equivalent to `(f a b)` |

### Fixtures

- [x] `tests/fixtures/currying/partial-basic.tur` -- `(f a)` on a 2-arg function
  produces the correct closure; calling the closure gives the right answer.
- [x] `tests/fixtures/currying/partial-chain.tur` -- `(((+ ) 3) 4)` gives `7`
  (operator section style).
- [x] `tests/fixtures/currying/over-apply.tur` -- `(f a b c)` where `f` returns
  a function; result is the composed call.
- [x] `tests/fixtures/currying/point-free.tur` -- `(map (+ 1) xs)` without an
  explicit `fn` wrapper.
- [x] `tests/fixtures/errors/currying-over-apply-bad.tur` -- over-applying past
  a non-function return type produces `TUR-E0002` with improved message.

**Exit criterion:** Fixtures exist and are documented; all currently produce
`TUR-E0002` (expected until CY1).

---

## Phase CY1 -- Under-saturated call sites (partial application)

**Goal:** When `elab_call_fn` detects that the number of provided arguments is
less than the function's arity, synthesise a closure instead of erroring.

### Changes to `elab_call_fn` (`src/elab.c`)

Currently (line ~7482):

```c
if (n_provided != expected_arity) {
    diag_emit(DIAG_ERROR, ..., "TUR-E0002 ...");
    return NULL;
}
```

New logic:

```c
if (n_provided < expected_arity) {
    return elab_partial_apply(e, call, fn_binding, elab_args, n_provided);
}
```

### New function: `elab_partial_apply`

Synthesises an anonymous function that closes over the supplied arguments and
calls the original function when given the remaining ones.

Given `f : (-> A B C)` called as `(f a)`:

1. Collect elaborated supplied args: `[a_expr]` with types `[A]`.
2. Compute remaining parameter types: `[B]` with result `C`.
3. Build a fresh `fn` form in the arena equivalent to:
   `(fn [b] :C (f a b))` -- but using `EX_VAR` / `EX_CALL` nodes directly,
   not re-parsing source text.
4. The synthesised `fn` has `n_captures = n_provided` (the supplied args are
   its free variables).
5. Call the existing `elab_fn` path to lift this into a closure binding.
6. Return `EX_CLOSURE` with the synthesised closure.

Type of the result:

```c
// Remaining argument kinds
TypeKind rem_arg_kinds[MAX_FN_ARITY];
for (uint8_t i = n_provided; i < arity; i++)
    rem_arg_kinds[i - n_provided] = fn_type.as.fn.arg_kinds[i];
uint8_t rem_arity = arity - n_provided;
TypeKind result_kind = fn_type.as.fn.result_kind;
Type pap_type = type_fn(rem_arg_kinds, rem_arity, result_kind);
```

### Interaction with full type annotations

When `f` has `arg_full_types` (rank-2 poly params), the synthesised closure
must carry the remaining full-type slots. Copy
`fn_type.as.fn.arg_full_types[n_provided..]` into the new `FnDef`.

### Diagnostics

- No new error code. Under-saturation is now valid.
- Over-saturation of a non-function return type still emits `TUR-E0002`; improve
  the message: *"function returns `int`, which is not callable -- did you mean to
  pass all N arguments?"*

### Tasks

- [x] Implement `elab_partial_apply` in `src/elab.c`.
- [x] Remove the `n_provided < expected_arity` error branch; replace with
  `elab_partial_apply` call.
- [x] Unit test: `partial-basic.tur` and `partial-chain.tur` fixtures pass.
- [x] Verify that fully-saturated calls are unaffected (no regression in existing
  fixtures).

**Exit criterion:** `partial-basic.tur` and `partial-chain.tur` pass; all
existing tests continue to pass.

---

## Phase CY2 -- Over-application

**Goal:** Allow `(f a b c)` where `f : (-> A B)`, `(f a b) : C`, and `C` is
itself a function type -- the elaborator chains the calls.

### Changes to `elab_form` / `elab_call_fn`

After computing the result expression for a fully-saturated or partially-
saturated call, check whether there are remaining arguments **and** the result
type is callable (`result_kind == TY_FN` or a `TY_PTR_VOID` closure):

```
remaining_args = call->as.list.len - 1 - n_consumed
if remaining_args > 0 and result_type is callable:
    synthesise a new call form: (result_expr remaining_args...)
    elab_form recursively
```

This naturally handles chains of arbitrary depth without special-casing.

### Edge case: result type unknown at compile time

If the result type is `TY_PTR_VOID` (a callback / opaque closure), over-
application emits a dynamic dispatch call through the existing `TY_PTR_VOID`
call path in `emit.c` (lines ~2402–2434). No change needed there.

### Tasks

- [x] Add over-application tail-recursion in `elab_call_fn`.
- [x] `over-apply.tur` fixture passes.
- [x] `currying-over-apply-bad.tur` error fixture emits correct `TUR-E0002`.

**Exit criterion:** `over-apply.tur` passes; error fixture produces the
improved diagnostic.

---

## Phase CY3 -- Standard library integration

**Goal:** Update `stdlib/` functions that take higher-order arguments so they
work naturally with curried calls. No type-system changes needed -- just
signature adjustments and new fixtures.

### Functions to audit

| File | Function | Current signature | Currying-friendly? |
|---|---|---|---|
| `stdlib/list.tur` | `map` | `(-> (-> :int :int) (List :int) (List :int))` | No -- specialised to `int` |
| `stdlib/list.tur` | `filter` | similar | No |
| `stdlib/list.tur` | `fold` | `(-> (-> :int :int :int) :int (List :int) :int)` | No |
| `stdlib/vec.tur` | `vec/map` | specialised | No |

These are currently monomorphic (all `int`). Generalising them to typeclass-
polymorphic signatures is a separate task (requires HKT `Functor`/`Foldable`).
For CY3, the goal is narrower:

- Confirm that `(map (+ 1) xs)` works once `+` can be partially applied.
- Add a `(curry f)` helper macro that wraps a 2-arg function into a 1-arg
  function returning a 1-arg function, for use where explicit currying syntax
  is cleaner than partial application.

### `curry` macro

```turmeric
;;; curry -- convert a 2-argument function to its curried form.
;;;
;;; Parameters:
;;;   f -- a 2-argument function
;;;
;;; Returns:
;;;   A 1-argument function that, given its first argument, returns a
;;;   1-argument function that applies f to both.
;;;
;;; Example:
;;;   (defn add [a b] :int (+ a b))
;;;   (let [add1 ((curry add) 1)]
;;;     (add1 41))  ; => 42
;;;
;;; Since: CY3
(defmacro curry [f]
  `(fn [a] : ... (fn [b] : ... (~f a b))))
```

(The `:...` return-type placeholders are resolved by the elaborator from `f`'s
declared type once type inference is in place.)

### Tasks

- [x] Write `point-free.tur` fixture using `(map (+ 1) xs)`.
- [x] Add `curry` macro to `stdlib/macros.tur`.
- [x] Add `curry` fixture: `currying-curry-macro.tur`.
- [x] Audit `list.tur` / `vec.tur` higher-order functions; note which need
  generalisation and file follow-up tasks. See *Stdlib HOF audit* below.

### Stdlib HOF audit (CY3)

At the time of the audit, `list.tur` and `vec.tur` had **no** dedicated
`list-map`/`list-filter`/`list-fold` or `vec-map`/`vec-filter`/`vec-fold`
entry points; the only higher-order entries in those files were:

| File | Function | Higher-order arg position | Notes |
|---|---|---|---|
| `list.tur` | `list-eq?` | last (`cmp-fn`) | comparator used internally for `Eq [Cons]` |
| `list.tur` | `__cons-fmap` | last (`f :fn`) | internal `Functor [Cons]` helper |
| `vec.tur` | `vec-eq?` | last (`cmp-fn`) | comparator used internally for `Eq [Vec]` |

These work with currying as written -- the trailing function/comparator
slot can be partial-applied -- but the more common pattern (`(map f xs)`
with the function *first* so `(map (+ 1))` becomes a section) is not
expressible until the polymorphic, function-first list/vec helpers exist.

Follow-up tasks (out of scope for CY4):

1. Add `list-map`, `list-filter`, `list-fold` to `list.tur` with the
   `(fn ... xs)` argument order (function first). Once these exist,
   `(map (+ 1) xs)` works out of the box via partial application.
2. Add `vec-map`, `vec-filter`, `vec-fold` to `vec.tur` with the same
   ordering convention.
3. Generalise these from `int -> int` over `(Functor f)` / `(Foldable f)`
   typeclasses (depends on HKT typeclasses).

**Exit criterion:** `point-free.tur` passes; `curry` macro is in stdlib and
documented.

---

## Phase CY4 -- Effect rows and rank-2 interaction

**Goal:** Ensure partial applications correctly propagate effect rows and that
rank-2 polymorphic functions (`forall`-quantified parameters) interact soundly
with partial application.

### Effect rows

When `f : (-> A B C) #{IO}` is partially applied as `(f a)`, the resulting
closure `g : (-> B C)` should carry the same effect row `#{IO}`. Currently the
synthesised closure in `elab_partial_apply` has no effect annotation. Fix:

- Copy `fn_type.as.fn.effect_row` onto the synthesised `FnDef`'s effect row.
- The effect-check pass (`effect_check.c`) will then propagate it from the
  closure's call site, as it does for any other closure.

### Rank-2 parameters

`(forall [a] (-> (-> a a) a a))` -- a function taking a polymorphic function
argument. If this is partially applied, the resulting closure must retain the
`forall` binder. This requires:

- Carrying `arg_full_types` through `elab_partial_apply` (partially handled in
  CY1; complete it here).
- Adding a fixture: `currying-rank2-partial.tur`.

### Tasks

- [x] Propagate `effect_row` through `elab_partial_apply`.
- [x] Propagate `arg_full_types` fully through partial application.
- [x] Fixture: `currying-effect-partial.tur` -- partial application of an
  effectful function retains the effect annotation.
- [x] Fixture: `currying-rank2-partial.tur` -- partial application of a rank-2
  function.

**Exit criterion:** Both fixtures pass; effect-check pass produces no spurious
`TUR-E0009` warnings for partially-applied effectful functions.

---

## Open Questions

1. **`MAX_FN_ARITY = 8`** -- the partial-application closure's remaining
   parameter list must also fit within 8 args. This is automatically satisfied
   since `rem_arity = arity - n_provided < arity <= 8`. No change needed.

2. **Type inference for `:...`** -- the `curry` macro uses placeholder return
   types. Full return-type inference is not currently in scope; for CY3, the
   macro requires explicit annotations or a future type-inference phase.

3. **Performance** -- each partial application allocates a heap closure. A
   future optimisation pass could detect when the partial-application closure
   is immediately fully applied (e.g., `((f a) b)`) and collapse it to a direct
   call with no allocation. This is out of scope for CY0–CY4.

4. **`defn` currying** -- should `(defn f [a b] :int ...)` automatically define
   a curried entry point `f/1 : (-> A (-> B C))`? This would allow `f` to be
   used in point-free style without a call site. Deferred; no decision made.
