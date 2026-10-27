# HKT Opaque-Container Dispatch Plan (D0--D3)

> **Status:** D0 planned. D1--D3 follow.
>
> **Prerequisites:** Phase H (HKT, H0--H8) complete. Dictionary passing (§1 of
> `docs/archive/hkt-deferred-tasks.md`) complete.
>
> **Related:** `docs/guides/hkt-guide.md` §"Known Limitations",
> `docs/archive/hkt-deferred-tasks.md` §1 (dictionary passing)

---

## Problem

The HKT guide documents this limitation:

> Method dispatch on containers: Since HKT container values are stored as
> `int64_t`, type-based dispatch may fall back to the first matching instance.
> Works reliably when only one typeclass instance is in scope for the given
> method.

### Root cause

All HKT container values (`option`, `vec`, `list`, ...) are represented as
opaque `int64_t` (a cast heap pointer). When a value crosses a function
boundary -- returned from a function, stored in a variable annotated `:int`,
or passed through a higher-order combinator -- its static type is erased.

At a later `.method` call site the elaborator (`elab_method_call` in
`src/elab.c`) tries to recover the type from `obj->type`. If `obj->type` is
`TY_INT` (the opaque fallback) the dispatch logic (`obj_ck == KIND_STAR`)
cannot distinguish `option` from `vec` -- both are `int64_t` at runtime. It
falls through to the "first-match" branch (~line 13485) which picks whichever
registered instance for that method appears first in `e->typeclass_env`.

### When this fires in practice

The ambiguous fallback is reached only when all of the following are true:

1. The receiver of `.method` has type `TY_INT` (erased type).
2. Two or more registered `definstance` blocks implement the same method name.
3. None of the instances fails the constraint check.

Programs that use exactly one HKT container type throughout, or that call
implementation functions directly (`__fmap_option`, `__bind_vec`, ...), are
not affected.

### Concrete failure case

```turmeric
(definstance Functor [option]
  (fmap [c f] (__fmap_option c f)))

(definstance Functor [vec]
  (fmap [c f] (__fmap_vec c f)))

;; Returns an option, but the return type annotation is :int.
(defn mk [] : int (__opt_some 42))

;; obj has type TY_INT at this call site.
;; First-match picks Functor[option] -- happens to be correct here only
;; because it was registered first.
(let [obj (mk)]
  (.fmap obj (fn [x] (* x 2))))   ;; silent wrong result if vec registered first
```

---

## Approach

Four phases, each independently shippable. D0 is the highest-priority fix
because it turns a silent wrong result into a visible diagnostic. D1 and D2
give users escape hatches. D3 is a compiler-assisted prevention layer.

| Phase | Deliverable | Cost | Risk |
|---|---|---|---|
| D0 | Diagnostic: error on ambiguous first-match | Low | None -- pure diagnostic |
| D1 | Type witness syntax `@Type` at call sites | Medium | Low |
| D2 | `box` existential stdlib wrapper | Medium | Low |
| D3 | Static dataflow warning at erasure sites | High | Medium |

---

## Phase D0 -- Diagnostic: Make Ambiguous Dispatch Visible

**Goal:** Replace the silent wrong-instance selection with a compiler error
when the fallback fires and more than one instance could match.

### Tasks

#### `src/elab.c` -- `elab_method_call`

- [ ] In the first-match fallback branch (~line 13485), count how many
  instances match the method name with `TY_INT`-compatible type args.
- [ ] If `count == 1`: accept the match silently (unambiguous fallback).
- [ ] If `count > 1`: emit `DIAG_ERROR`:

  ```
  error: ambiguous method dispatch: `.fmap` matches N instances
  (Functor[option], Functor[vec], ...) -- receiver type is erased (int64_t).
  Hint: annotate the receiver's type or use `@TypeName` syntax (see D1).
  ```

- [ ] If `count == 0` and no exact match was found: keep existing
  "method not found" error.

#### Diagnostics (`src/diag.h`)

- [ ] Reserve a new error code, e.g. `TUR_E0015_AMBIGUOUS_DISPATCH`, for the
  new diagnostic.

#### Documentation

- [ ] Update `docs/guides/hkt-guide.md` §"Known Limitations" to describe the
  new error and link to this plan.

### Fixtures

- [ ] `tests/fixtures/hkt-dispatch-ambiguous/` -- two Functor instances, erased
  receiver; expect error `TUR_E0015_AMBIGUOUS_DISPATCH`.
- [ ] `tests/fixtures/hkt-dispatch-unambiguous/` -- one Functor instance, erased
  receiver; expect the fallback silently accepted and correct output.
- [ ] All existing HKT fixtures continue to pass (no regression).

### Exit criterion

`TUR_E0015_AMBIGUOUS_DISPATCH` is emitted on every ambiguous fallback;
single-instance programs produce no new errors.

---

## Phase D1 -- Type Witness Syntax `@Type` at Call Sites

**Goal:** Allow the programmer to explicitly name the type constructor at a
`.method` call site, resolving ambiguity at compile time with zero runtime
cost.

### Syntax

```turmeric
(.fmap @option obj (fn [x] (* x 2)))
```

The `@TypeName` token immediately after the method name is a *type witness*.
It names the typeclass instance to use. The compiler resolves the witness to a
dictionary at elaboration time and emits a direct call through that dictionary,
regardless of `obj`'s static type.

#### Surface grammar addition (`src/reader.c`)

- [ ] Recognise `@name` (ASCII `@` followed by an identifier) as a new token
  kind `TOK_WITNESS` inside a list head position.
- [ ] In `elab_method_call`, check whether `call->as.list.items[1]` is a
  `TOK_WITNESS`. If so, extract the type name and shift arguments accordingly.

#### Elaborator (`src/elab.c`)

- [ ] When a witness `@T` is present, look up the instance by method name +
  type name directly, bypassing the type-based search.
- [ ] Emit the same `EX_DICT` node as for the unambiguous case.
- [ ] Error if `@T` names a type that has no instance for the given method:

  ```
  error: no instance of `Functor` for type `pair` at `.fmap @pair`
  ```

#### Restriction

- [ ] `@Type` is only meaningful at `.method` call sites; using it elsewhere
  is a parse error.

### Fixtures

- [ ] `tests/fixtures/hkt-witness-basic/` -- `.fmap @option` on an
  erased-type receiver selects `Functor[option]`.
- [ ] `tests/fixtures/hkt-witness-wrong-type/` -- `.fmap @pair` where no
  `Functor[pair]` instance exists; expect error.
- [ ] `tests/fixtures/hkt-witness-multi-instance/` -- two instances, erased
  receiver, `@vec` and `@option` witnesses select the correct implementation
  and produce correct output.

### Exit criterion

`@Type` syntax resolves ambiguous dispatch at zero runtime cost; existing
tests pass; the D0 error is suppressed when a witness is present.

---

## Phase D2 -- Existential `box` Wrapper

**Goal:** Provide a stdlib-level wrapper that bundles an HKT container value
with its dictionary pointer, so that dispatch through an opaque `int64_t`
boundary is always unambiguous -- at the cost of one extra heap allocation and
one pointer dereference per dispatch.

This is analogous to Haskell's existential types or Rust's `Box<dyn Trait>`.

### Design

#### Runtime layout

```c
typedef struct {
    void        *dict;   /* pointer to the resolved dictionary struct  */
    int64_t      value;  /* the opaque HKT container (e.g. option<int>) */
} TurBox;
```

`box` values are heap-allocated. Their `int64_t` handle is a cast pointer to
`TurBox`. The `dict` field is set at box-construction time, when the static
type is known.

#### Turmeric surface

```turmeric
;; Boxing (static type must be known at this point)
(let [b (box @option (__opt_some 42))]
  ;; b has type TurBox, stored as :int

  ;; Dispatch through the box -- no type information needed at call site
  (.fmap b (fn [x] (* x 2))))
  ;; => some 84
```

#### Implementation (`stdlib/box.tur`)

- [ ] `(defn box-make [dict :int value :int] :int ...)` -- allocate a `TurBox`,
  store dict pointer and value.
- [ ] `(defn box-value [b :int] :int ...)` -- extract the inner value.
- [ ] `(defn box-dict [b :int] :int ...)` -- extract the dict pointer (for
  advanced use only).
- [ ] `(defmacro box [@T value] ...)` -- sugar that resolves the `@T` witness
  to a dict pointer at compile time and calls `box-make`.

#### Elaborator changes (`src/elab.c`)

- [ ] When `.method` is called on a receiver whose type is `TY_INT`, check
  whether `obj` originated from `box-make` (track via a new `EX_BOX` IR node,
  or by tagging the origin in `Expr`).
- [ ] If so, emit a runtime dict-pointer dereference: load `((TurBox*)obj)->dict`,
  then indirect-call through the dict's method field. This is one extra load vs.
  the compile-time path.
- [ ] For non-box `TY_INT` receivers, emit `TUR_E0015_AMBIGUOUS_DISPATCH` as
  before (D0).

#### IR (`src/expr.h`)

- [ ] Add optional `EX_BOX_UNPACK` node: given an `int64_t` that is a `TurBox*`,
  emit `((TurBox*)(intptr_t)val)->method_ptr(...)`.

### Fixtures

- [ ] `tests/fixtures/hkt-box-basic/` -- box an option, call `.fmap` on it.
- [ ] `tests/fixtures/hkt-box-across-boundary/` -- box an option, pass as
  `:int` parameter to another function, call `.fmap` on the parameter.
- [ ] `tests/fixtures/hkt-box-two-types/` -- box an option and a vec, store
  both as `:int`, call `.fmap` on each -- both dispatch correctly.

### Exit criterion

`box` values can be passed through `:int`-typed parameters and still dispatch
correctly; one pointer dereference per dispatch; D0 error is suppressed for
`box` receivers.

---

## Phase D3 -- Static Dataflow Warning at Type-Erasure Sites

**Goal:** Warn at the *source* of type erasure (function returns, `:int`
variable annotations, struct field stores) when the erased value might later
require typeclass dispatch and the dispatch would be ambiguous.

This is a best-effort static analysis; it does not need to be sound to be
useful.

### Detection heuristic

An erasure site is suspicious when:
1. The value being erased has a non-primitive type (i.e. it was known to be
   an HKT container at elaboration time).
2. The erased value flows into a context where a `.method` call is reachable.
3. More than one instance of the relevant typeclass is in scope.

### Implementation (`src/flow_warn.c` -- new file)

- [ ] Add a lightweight taint pass after elaboration:
  - Tag any `Expr` whose static type is a known HKT constructor as
    `TAINT_HKT(constructor_id)`.
  - When such an expr is cast to `TY_INT` (return type coercion, explicit cast,
    parameter passing), record the erasure span and taint the resulting `int64_t`
    binding with `TAINT_ERASED_HKT(constructor_id)`.
  - When a `.method` call is elaborated on a `TAINT_ERASED_HKT` receiver,
    check whether the method has multiple registered instances. If so, emit
    `DIAG_WARN`:

    ```
    warning: `.fmap` is called on a value whose container type was erased at
    <original-span>. Dispatch will use the first-match fallback.
    Hint: return `@option` from the function, or use `box @option` at
    <original-span>.
    ```

- [ ] Gate the taint pass behind a flag, e.g. `--warn-erased-dispatch`, so
  it does not slow down normal compilation.

#### Limitations to document

- The analysis is intraprocedural: it does not track erasure across module
  boundaries or through arbitrary function arguments.
- It does not warn if the `.method` call is in a different function from the
  erasure site (interprocedural taint would require a separate analysis pass).

### Fixtures

- [ ] `tests/fixtures/hkt-erasure-warn/` -- function that returns `:int` from
  an option-producing expression; `.fmap` called on the result with multiple
  instances in scope; expect `DIAG_WARN`.
- [ ] Verify `--warn-erased-dispatch` is off by default (no new warnings in
  existing test suite).

### Exit criterion

`--warn-erased-dispatch` surfaces at least the single-function erasure case;
no false positives on the existing test suite.

---

## Non-Goals

- **Full interprocedural type inference** -- recovering the erased type from
  call graphs is left to a future whole-program optimizer.
- **Changing the runtime representation of all HKT containers** -- adding a
  mandatory header word to every container allocation would touch all of stdlib
  and is not justified by the frequency of cross-boundary dispatch.
- **Automatic boxing** -- implicitly wrapping every HKT container in a `TurBox`
  at type-erasure sites would change performance characteristics silently; box
  must remain an explicit opt-in.

---

## Open Questions

1. **`@Type` witness scope** -- should witnesses be allowed in `do-m` and `for`
   desugaring sites? The macro expander would need to propagate them.
2. **Multi-parameter typeclasses** -- the `@Type` witness specifies only the
   first type argument. For classes with multiple type parameters (e.g.
   `Bifunctor[pair]`) the syntax may need to be `@(pair)` or `@pair` with
   inference filling in remaining args.
3. **D3 gate** -- should `--warn-erased-dispatch` eventually become a default
   warning (like `-Wshadow` in C)? Decide after measuring false-positive rate
   on the full stdlib.

---

## Estimated Effort

| Phase | Effort | Dependencies |
|---|---|---|
| D0 | Small (< 1 day) | None |
| D1 | Medium (2--3 days) | D0 (uses same instance lookup path) |
| D2 | Medium (3--4 days) | D1 (reuses `@Type` witness resolution) |
| D3 | Large (1 week) | D0, D2 |
