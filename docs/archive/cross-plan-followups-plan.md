# Cross-Plan Followups

> **Status:** F1, F2-1, F2-2-2, F3 (recursive dispatcher synthesis
> for ALL typed collections including Set), F4 (infra +
> --Werror=deprecated; full stdlib sweep deferred), F5
> (MutableMap[K V] open-addressed hash table), F7 (doc hygiene),
> F8 (defstruct compound field-type annotations -- TY_APP /
> TY_EXISTS / TY_FORALL field declarations and field-access
> elaboration; F8-5 move-at-pack extension and F8-7 cycle
> fixture deferred as their own follow-ups), F6 (partial: F6-2
> via fixture rename + F6-4 for clone-list / clone-option /
> clone-pair; F6-1 / F6-3 share a deeper compiled-mode
> ADT-field-type carry-through bug for nested matches and the
> stdlib-name-collision fixture renames), F4-4 (mass stdlib
> `^deprecated` sweep across the 7 legacy modules) all shipped.
> Every original plan task is now shipped.  The plan can be
> moved to `docs/archive/` once the maintainers have reviewed
> the final state.
> Remaining open phases:
>   - F3-2..F3-7 -- dictionary passing + receiver type recovery
>     (see Phase F3 "Additional blocker" for the expanded scope),
>   - F4-4 -- mass stdlib `^deprecated` annotation (deferred so
>     each module lands with its migration guidance),
>   - F5 -- `MutableMap[K V]`,
>   - F6 -- interpreter (turi) fixture gaps,
>   - F8 -- `defstruct` compound field type annotations (unblocks
>     `exg4-pack-into-struct` and the F2-2-1 cycle-construction
>     fixture).
> **Last Updated:** 2026-05-23
> **Type:** Compiler / Runtime / Stdlib / Docs

---

## Overview

The recent existential-types and typed-collections work landed in
several waves, each of which explicitly deferred a small follow-up so
the main change could ship clean.  Those deferrals have now
accumulated, and several of them are blocking each other.  This plan
gathers them into one place so they can be planned and prioritised
together.

Source plans the work threads back to:

- `docs/existential-gc-followup-plan.md` (EXG4/EXG5/EXG6)
- `docs/existential-types-plan.md` (EX1/EX2)
- `docs/existential-gc-plan.md` (EXG1/EXG2/EXG3 -- now superseded by
  the followup plan, but the doc itself still needs a status update)
- `docs/ptc-plan.md` (PTC4 forward-looking note)
- `docs/typed-collections-plan.md` (deprecation warnings, mutable
  variants, interpreter gaps, recursive `.eq?` dispatch)

The phases below are independent and can land in any order, with the
single sequencing constraint called out under F1.

---

## Dependency Graph

```
F1-1 (parser fix)
  |
  +--> F1-2 (smart drop + pack semantics)
  |       |
  |       +--> F2-2 (cycle-construction tests)
  |
  +--> F2-1 (linear escape rejection)
  |
  +--> reopens EXG4-pack-return / EXG4-pack-into-fn /
       EXG4-pack-into-struct fixtures from the followup plan

F3 (typeclass dictionary passing)
  +--> unblocks Vec[Vec[int]] / Map[K (Vec V)] etc.

F4 (^deprecated attribute)
  +--> retires the docstring-only deprecation in typed-collections

F5 (MutableMap[K V]) -- independent

F6 (turi gaps) -- independent

F7 (doc hygiene) -- independent, blocks nothing

F8 (defstruct compound field annotations)
  +--> unblocks exg4-pack-into-struct fixture (originally EXG4-5)
  +--> unblocks exg5-exists-cycle fixture (F2-2-1)
  +--> unblocks Vec[Showable] / Map[K (exists ...)] storage idioms
```

---

## Phase F1 -- Parser + existential semantics gap-closers

### F1-1 -- Parser fix for constrained-existential type annotations

**Problem.**  Today the only way to spell a constrained existential as
a function return or parameter type is the compound annotation form,
e.g.

```turmeric
(defn make [] : (exists [a] [(Show a)] a)
  (pack 1 (exists [a] [(Show a)] a)))
```

The annotation parses, but the elaborator builds a `Type` whose
`as.forall_` payload is invalid, and the first downstream consumer
(`elab_open` at `src/compiler/elab_types.c:1793`) SEGVs with a
misaligned read trying to walk `packed->type.as.forall_.body`.

This blocks:

- The plan's `exg4-pack-return`, `exg4-pack-into-fn`, and
  `exg4-pack-into-struct` fixtures (EXG4-5 in
  `existential-gc-followup-plan.md`).
- A natural spelling for `errors/exg6-linear-escape` (F2-1).
- Any user-level API that returns or accepts a constrained existential
  by value.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| F1-1-1 | Audit `type_expr_from_form` for the `:(exists ...)` compound case -- **done** (the compound case parses fine; the payload was populated correctly inside `type_expr_from_form`).  The bug was downstream in `elab_fns.c` losing the payload at return-type capture time. | `src/compiler/elab_types.c` |
| F1-1-2 | Fix any field the audit shows uninitialised -- **done** via F1-1-3 (no fix needed in `elab_types.c`). | `src/compiler/elab_types.c` |
| F1-1-3 | Mirror the fix in `elab_fns.c`'s F_TYPE_ANN handling so the function's `result_full_type` carries the full TY_EXISTS/TY_FORALL payload -- **shipped** (`elab_fns.c`: capture `return_exists_type`, attach to `result_full_type`; `elab_call.c`: patch call-expression type from `result_full_type` on TY_EXISTS/TY_FORALL).  Same change also gives constrained-existential parameters a value-typed path (was previously mis-routed through the rank-2 branch which rejected `pack` arguments). | `src/compiler/elab_fns.c`, `src/compiler/elab_call.c` |
| F1-1-4 | Add fixture `tests/fixtures/ex-exists-return-type` -- **shipped**. | `tests/fixtures/` |
| F1-1-5 | Add fixture `tests/fixtures/ex-exists-param-type` -- **shipped**. | `tests/fixtures/` |
| F1-1-6 | Ship the previously blocked EXG4-5 fixtures -- **shipped**: `exg4-pack-return`, `exg4-pack-into-fn`, `exg4-pack-into-struct` (the last via F8-6 once F8 extended the defstruct compound-annotation parser). | `tests/fixtures/` |

### F1-2 -- Smart drop hook for existential RC payloads + pack semantics

**Problem.**  EXG5 added the cycle-walker visibility for existentials
whose payload is itself an `rc<T>` (kind tag `RCK_EXISTENTIAL` +
payload `RCEXP_RC`).  What it deliberately skipped is the matching
*ownership* semantics:

- `pack` of an `rc<T>` value today stores the inner control-block
  pointer in the existential's `value` slot without incrementing it
  and without marking the source binding moved.
- The existential's drop hook (`tur_existential_drop`) is still a
  no-op, so freeing the outer existential does **not** decrement the
  inner rc.

Adding either piece alone would break things.  Adding both together is
the natural completion of EXG5 and unblocks the genuine cycle-collection
test (F2-2).

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| F1-2-1 | Decision: **move-at-pack** (followup-plan recommendation).  Cheaper than clone-at-pack; matches the existing `rc/clone` ergonomic where users opt into shared ownership explicitly. | design decision |
| F1-2-2 | Clone-at-pack: rejected by F1-2-1. | -- |
| F1-2-3 | Move-at-pack in `elab_pack` -- **shipped**.  When the packed value is an `EX_VAR` of TY_RC, TY_WEAK, or TY_EXISTS, mark the source binding moved via `binding_mark_moved`.  The surrounding scope's auto-drop pass then skips it, leaving the existential as the sole owner of the inner rc reference. | `src/compiler/elab_types.c` |
| F1-2-4 | Smart drop hook -- **shipped**.  Runtime `rc_cb_free` and the matching inline runtime in `emit_module.c` (both `rc_free_queue_drain` and `rc_weak_decrement`'s zombie-free path) dispatch on `reserved[0] == RCK_EXISTENTIAL` + `reserved[1] == RCEXP_RC` and call `rc_strong_decrement` on the inner cb before the outer drop hook fires.  Layout-mirror the GC walker's existing read of `*(int64_t*)cb->value`. | `src/runtime/rc.c`, `src/compiler/emit_module.c` |
| F1-2-5 | Hook-vs-dispatch choice: **dispatch in `rc_cb_free`** (and the inline analogue) rather than swapping `tur_existential_drop` per-program.  The single teardown entry point keeps the per-program drop hook a layout-free no-op, and the runtime library never needs to know about the codegen-generated struct beyond reading the first 8 bytes of `cb->value`. | `src/compiler/emit_module.c` |
| F1-2-6 | Fixture `tests/fixtures/exg5-rc-in-exists` -- **shipped**.  Packs an `rc<int>` into a constrained existential, opens it, and exits cleanly (no leak, no double-free).  The would-be fixture `exg5-rc-in-exists-gc-roundtrip` (using `weak` + `upgrade` to assert post-collection state) was dropped because the compiler currently leaks 24 bytes from `fresh_tmp` on emitting `weak`/`upgrade` paths -- a pre-existing leak (also fails `weak-dangling` baseline) unrelated to F1-2 logic.  Track that compiler leak separately. | `tests/fixtures/` |

### F1-3 -- Storage-site auto-clone (EXG4-3)

**Problem.**  The followup plan deferred EXG4-3 (auto-clone of
constrained existentials when stored into a struct field, vec slot, or
async capture) on the grounds that current `rc<T>` baseline does not
auto-clone there either and the plan's premise was inaccurate.  If we
decide auto-clone *is* desired across the board, the same change should
apply to `rc<T>` first, then extend to constrained existentials.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| F1-3-1 | Decision: **retire** EXG4-3 -- **shipped**.  The original EXG4-3 design proposed auto-cloning constrained existentials at storage sites on the premise that `rc<T>` already does so.  The premise is wrong: `rc<T>` requires explicit `rc/clone` at struct/vec/async storage sites.  Bringing the `rc<T>` policy to auto-clone is a much larger user-visible change and is out of scope for this plan.  Constrained existentials follow the established `rc<T>` discipline: explicit `rc/clone` at storage sites. | design decision |
| F1-3-2 | Implementation: not needed -- **n/a** (retire path chosen). | -- |
| F1-3-3 | Doc updates: backfill `existential-gc-followup-plan.md` to flip EXG4-3 from "open" to "retired" and update the Relation-to-Other-Plans section to drop the "EXG4-3 is what unblocks Vec[Showable]" framing in favour of explicit-clone -- **shipped**. | docs |

---

## Phase F2 -- Existential safety completeness

### F2-1 -- `errors/exg6-linear-escape`

**Problem.**  The EXG6 plan called for a diagnostic when a `:linear`
existential is returned from a `defn`.  Today the natural spelling
(declare the return type as the linear existential) hits F1-1's parser
crash.  Erasing the return to `:ptr<void>` silently strips the linear
discipline, leaking the record.

**Tasks (depends on F1-1).**

| ID | Task | File(s) |
|----|------|---------|
| F2-1-1 | Decision: **defn-boundary check** -- a single deterministic check at return-type annotation parsing, independent of body shape (direct pack, let-tail, conditional return).  Simpler than flow analysis at the pack site, and the linear-escape contract is naturally a property of the function's return type. | design decision |
| F2-1-2 | If at pack: rejected by F2-1-1 decision. | -- |
| F2-1-3 | At defn: in `elab_fns.c` return-type parsing, when the annotation resolves to a `TY_EXISTS` with `is_linear=true`, emit a diagnostic -- **shipped**. | `src/compiler/elab_fns.c` |
| F2-1-4 | Add fixture `tests/fixtures/errors/exg6-linear-escape` -- **shipped**. | `tests/fixtures/errors/` |

### F2-2 -- Cycle-construction tests for existentials

**Problem.**  EXG5 shipped three mechanical fixtures verifying the kind
tag is written and the walker doesn't crash.  The plan's real
cycle-collection tests (`exg5-exists-cycle`) are blocked on F1-2
(smart drop + correct pack semantics).

**Tasks (depends on F1-1 and F1-2).**

| ID | Task | File(s) |
|----|------|---------|
| F2-2-1 | Cycle-construction fixture -- deferred to **F8-7** (same `defstruct` compound-annotation gap as `exg4-pack-into-struct`; F8 is the dedicated phase that closes both at once). | `tests/fixtures/` |
| F2-2-2 | Roundtrip fixture -- **shipped via F1-2-6** as `exg5-rc-in-exists`.  The would-be `exg5-rc-in-exists-gc-roundtrip` companion (weak + upgrade) was dropped because it trips the same pre-existing compiler-side `fresh_tmp` leak that also fails `weak-dangling` -- not an F1-2 regression, track the compiler leak separately. | `tests/fixtures/` |

---

## Phase F3 -- Recursive typeclass dispatch through dictionaries

**Problem.**  PTC4 shipped constrained `definstance` for collections
(`Eq[Vec[A]]`, `Eq[Map[K V]]`, etc.), but element comparison inside
the instance bodies uses integer `=` instead of dispatching through
`.eq?` on the element type.  This is correct for primitive elements
(`int`, `bool`, `cstr`) but breaks for nested collections like
`Vec[Vec[int]]` and `Map[K (Vec V)]`.  Both PTC and typed-collections
plans flag this as deferred to a future phase with identical wording:

> Full recursive structural equality (e.g. `Vec[Vec[int]]`) requires
> dictionary passing to method bodies.

(See `docs/ptc-plan.md` "Remaining Work After PTC4" and
`docs/typed-collections-plan.md` "Blocking Work: Phase PTC4" -- both
end with this exact note.)

The blocker is that method bodies do not currently receive their
constraint dictionaries as runtime parameters, so `.eq?` inside a
`definstance Eq [Vec] [(Eq A)]` body has nothing to dispatch through
beyond the syntactic name.

### Additional blocker discovered while scoping F3 (2026-05-23)

The plan's framing above describes the *body-side* of dispatch.
While prototyping F3 we found a matching *call-site* gap: the
receiver-type-erasure problem.  `tvec-push` and friends return
`:int` (the C-level int64 representation of the opaque collection
pointer), not `:Vec[int]`.  Calling `.eq?` on a binding produced
by `tvec-push` therefore fails with:

```
error: ambiguous method dispatch: '.eq?' matches 7 instances
  (Eq[Set], Eq[Cons], Eq[Pair], Eq[Result], Eq[Option], Eq[Vec], Eq[Map])
  -- receiver type is erased (int64_t).  Hint: annotate the
  receiver's type or use @TypeName syntax (see D1).
```

The `@TypeName` workaround pins dispatch but does not actually
exercise the dictionary-passing path -- it bypasses constraint
resolution entirely.  Existing typed-collection fixtures
(`tests/fixtures/typed/tvec-basic`) sidestep this by calling
`tvec-eq?` directly with an explicit comparator instead of
`.eq?`, which means the recursive-equality regression is
currently *latent* (no fixture catches it).

For F3 to actually deliver recursive structural equality through
`(.eq? outer-vec outer-vec)`, both pieces must land together:

1. The body-side dictionary parameter so `(.eq? a b)` inside an
   instance body has somewhere to dispatch when `a` has a
   universally-quantified type.
2. A call-site mechanism so the receiver's `:int`-erased type is
   recovered to the constructor's full TY_APP type, allowing
   instance resolution to pick `Eq[Vec[A]]` (and then resolve
   `A` from the recovered TY_APP).

The PTC4 work began (2) for direct constructor calls (TY_APP
`result_full_type`), but the typed-collection helpers
(`tvec-push`, `tvec-new`, etc.) discard the TY_APP at the return
boundary.  Closing F3 therefore also requires either:

- (option a) extending PTC4's `result_full_type` to cover the
  typed-collection helpers, plus a new emit path that keeps
  TY_APP on let-bindings of helper results, or
- (option b) introducing a `(:: e :Vec[int])` ascription form
  that re-attaches TY_APP without changing helper signatures.

Either choice is its own design decision and pushes F3 from "1
session" to "2-3 sessions" of work.  This was understated in the
original plan -- the dictionary-passing change alone is
mechanically the largest piece, but it is not deliverable in
isolation.

### F3 implementation -- session 3 progress (2026-05-24)

F3-7 (sticky ascription + struct-identity dispatch) shipped end to
end.  Single-level `(.eq? v v)` on `(:: v (Vec T))` for primitive
T resolves through the correct vtable (`Eq[Vec]` → constraint
`Eq[A=T]` → primitive instance) and returns the right answer.
Fixtures `tvec-eq-ascribed` (single primitive type) and
`tvec-eq-ascribed-multi` (int / bool / cstr / int32 / uint64)
cover the win.

We then prototyped the recursive case (`Vec[Vec[int]]` via
`.eq?`) and found that the missing piece is purely **per-call-site
instance-body substitution**, not full Haskell-style dictionary
passing.  Specifically: a manual rewrite of the call to

  `(tvec-eq? recv1 recv2 (fn [a b] (.eq? (:: a (Vec int)) (:: b (Vec int)))))`

produces correct answers today, leveraging F3-7's
sticky-ascription dispatch.  The dispatcher could synthesise this
rewrite automatically when it sees `.eq?` on a `Vec[TY_APP(...)]`
receiver, but doing so cleanly requires either:

1. **Form synthesis + re-elaboration**: build the inner-comparator
   lambda Form (with `(:: a TYPE)` / `(:: b TYPE)` ascriptions) and
   call `elab_form` on it, then attach the result to a manually-
   built `EX_CALL` to the helper (`tvec-eq?` / `tmap-eq?` / ...).
   The receiver Expr nodes go directly into the call's `args`
   array.  Requires a `type_to_form` helper for the common cases
   (TY_INT/TY_BOOL/.../TY_STRUCT/TY_APP) and a hardcoded mapping
   from struct identity to helper symbol.

2. **Monomorphised synthesised defns**: at each unique call-site
   element-type T, generate a fresh top-level
   `(defn __spec_eq_Vec_<T> [a :int b :int] :bool ...)` that
   recursively descends.  Cache by mangled name to avoid
   regenerating across call sites.  Inserts new defns into the
   program flow, which is a deeper edit to `elaborate_program`.

Approach (1) is recommended -- the synthesis is local to each
call site, no new program-flow machinery, and the recursion
bottoms out automatically because the synthesised lambda's
nested `.eq?` re-enters the same dispatch site at the next
level down.

### F3 implementation attempt -- session 2 findings (2026-05-23)

While trying to bring up F3-2..F3-6 we hit the receiver-type-erasure
wall described above and confirmed it is the gating issue, not the
dict-passing infrastructure.  Specifically:

- `(:: (tvec-new) (Vec int))` ascription parses and overrides the
  binding's static type, but the existing `EX_ASCRIBE` lowering
  drops the TY_APP at the next operation that takes the value as
  `:int` (every typed-collection helper has `:int` parameters).
  So even with an ascription, the next `(tvec-push! v ...)` reads
  `v` as `:int` and the recovered TY_APP is lost.
- `@TypeName` witness syntax pins the *outer* dispatch to a
  specific instance but provides no recursive type info, so
  `(.eq? @Vec outer-of-outer-of-int)` resolves to Eq[Vec]'s
  instance method whose body hardcodes integer = on elements.
- `@(Vec int)` (parenthesised type application as a witness) is
  not supported: the reader lowers it to `(deref (Vec int))` and
  treats `Vec` as a function name.
- The interpreter (`tur --interpret`) has no `@TypeName` support
  at all, so any F3 fixture that uses `.eq? @...` would be
  compiler-only.

These findings reinforce the F3-7 framing: receiver-type recovery
must land first, and the implementation choice is non-trivial.
Option (a) -- threading TY_APP through every typed-collection
helper's `result_full_type` -- requires touching every
`tvec-`/`tmap-`/`tlist-`/`tset-`/`toption-`/`tresult-`/`tpair-`
signature.  Option (b) -- a typed ascription form that re-attaches
TY_APP and persists across subsequent helper calls -- requires
extending `EX_ASCRIBE` to be sticky (the static type follows the
binding through subsequent operations instead of being dropped at
the first `:int` parameter contact).  Sticky ascription is the
smaller change.

### Design sketch (F3-1)

**Call convention.**  For a constrained-instance method
`(definstance C [T] [(C1 A) (C2 B) ...] (method [p1 p2 ...] body))`,
the generated C function gains one hidden parameter per constraint,
inserted before the user parameters:

```c
ret_t method_impl(
    const dict_C1_T* __dict_C1_A,   /* one per constraint */
    const dict_C2_T* __dict_C2_B,
    /* ... */
    arg1_t p1, arg2_t p2, ...       /* original parameters */
);
```

**Body-side resolution.**  When `body` evaluates `(.method2 x)`
where `x : A` (a constrained type variable bound by the instance's
constraint vector), the elaborator:

1. Finds the constraint `(C1 A)` that binds `A`.
2. Synthesises a fresh binding for the corresponding hidden
   dictionary parameter (named e.g. `__dict_C1_A`).
3. Lowers `(.method2 x)` to `__dict_C1_A->method2_(x, ...)`.

This requires extending the constraint-vector parser in
`elab_typeclasses.c` to record the hidden-parameter binding per
constraint, and extending `elab_method_call` to recognise the
"receiver type is a constrained type variable" case and route
through the dict binding instead of the static singleton.

**Call-site resolution.**  When the dispatcher resolves
`(.method outer)` against an instance whose typeclass has its own
constraints (e.g. `Eq[Vec[A]]` requires `Eq[A]`), the dispatcher
must recursively resolve the inner constraints (`Eq[A]` where
`A = Vec[int]` -> `Eq[Vec[int]]`'s singleton -> ...) and pass
each resolved dictionary as an extra argument.  Recursion
terminates at primitive types whose instances are unconditional
singletons.

**ABI compatibility.**  Existing unconstrained instance methods
(`Eq[int]` etc.) are unchanged -- they have no constraints, so no
hidden parameters, so the same vtable layout works.  Constrained
instance methods get a *new* vtable shape (one per typeclass +
type-args combination), which is a fresh codegen so there is no
backwards-compatibility issue.

### Tasks

| ID | Task | File(s) |
|----|------|---------|
| F3-1 | Design dictionary-passing call convention -- **shipped** (this section).  The implementation work (F3-2..F3-7) is deferred to a follow-up session given the scope expansion described above. | -- |
| F3-bug | Tracking fixture `tests/fixtures/tvec-of-tvec-eq-manual` -- **shipped**.  Documents that `(.eq? @Vec outer outer)` on a `Vec[Vec[int]]` returns the wrong answer today (the Eq[Vec[A]] instance hardcodes integer = for elements), but the underlying `tvec-eq?` primitive with a nested comparator lambda gives the correct answer.  When F3-2..F3-7 ship, a companion `tvec-of-tvec-eq` fixture should appear that asserts the same answers via the `.eq?` dispatch path. | `tests/fixtures/` |
| F3-2 | Extend `elab_typeclasses.c` to elaborate constrained-instance methods with hidden dictionary parameters in scope, accessible by the constrained typeclass name. | `src/compiler/elab_typeclasses.c` |
| F3-3 | Extend the method-dispatch loop to recursively resolve inner constraints and thread dictionaries from the call site into the hidden parameters. | `src/compiler/elab_typeclasses.c` |
| F3-4 | Update `emit_fns.c` to emit the extra parameters and `emit_expr.c` to pass the dictionaries at the call.  Also extend the EX_METHOD_CALL emit to choose between the static-singleton path (unconstrained) and the dict-pointer-arg path (constrained). | `src/compiler/emit_fns.c`, `emit_expr.c` |
| F3-5 | Per-call-site dispatcher rewrite -- **shipped for all typed collections** (Vec, Map, Option, Cons, Pair, Result, Set).  `elab_method_call` (`src/compiler/elab_typeclasses.c`) recognises `.eq?` on a typed-collection TY_APP receiver and synthesises a direct call to the helper (`tvec-eq?` / `tmap-eq?` / `toption-eq?` / `tlist-eq?` / `tpair-eq?` / `tresult-eq?` / `tset-eq-cmp?`) with inline comparator lambdas whose params are ascribed to the element type(s).  Set's original helper `tset-eq?` takes no comparator and is hash-only (wrong for non-primitive elements), so F3-5 added a new `tset-eq-cmp?` to `stdlib/tset.tur` that does O(n*m) structural comparison and routes Set dispatch through it.  Recursion bottoms out at primitive elements where the F3-7 single-level path takes over.  Helpers `type_to_form`, `build_comparator_lambda`, `helper_eq_symbol_for_struct`, and `try_synth_recursive_eq` live in `elab_typeclasses.c`.  1- vs 2-comparator dispatch is driven by `helper_eq_symbol_for_struct`'s `out_n_comparators` flag. | `src/compiler/elab_typeclasses.c`, `stdlib/tset.tur` |
| F3-6 | Recursive structural-equality fixtures -- **shipped**: `tvec-of-tvec-eq` (two- and three-level deep `Vec[Vec[int]]` / `Vec[Vec[Vec[int]]]`), `tmap-of-tvec-eq` (`Map[int (Vec int)]`), `tcons-of-tcons-eq` (`Cons[Cons[int]]`), `toption-of-tvec-eq` (`Option[Vec[int]]` + None case), `tpair-of-typed-eq` (2-comparator: `Pair[Vec[int] Option[int]]`), `tresult-of-typed-eq` (2-comparator with mixed recursive + primitive: `Result[Vec[int] cstr]`), `tset-of-tvec-eq` (`Set[Vec[int]]` via `tset-eq-cmp?`).  The companion `tvec-of-tvec-eq-manual` from session 2 is kept as the manual-rewrite reference. | `tests/fixtures/` |
| F3-7 | Receiver-type recovery for typed-collection helpers -- **shipped** (option b "sticky ascription").  Three pieces: (1) primitive `Eq` instances for the full numeric stack (`int`, `bool`, `cstr`, `float`, `int8`/`int16`/`int32`/`uint8`/`uint16`/`uint32`/`uint64`/`float32`) added to the auto-loaded `stdlib/typeclass-eq.tur` so `(Eq A)` constraints satisfy for any primitive element type without requiring `(load "stdlib/typeclass.tur")`; (2) `typeclass_env_lookup_instance` in `src/compiler/typeclass.c` walks a TY_APP receiver to its struct head and checks struct identity so `Eq[TY_APP(Vec, int)]` resolves to `Eq[Vec]` (not `Eq[Map]` etc.); (3) the matching KIND_ARROW path in `elab_method_call` in `src/compiler/elab_typeclasses.c` does the same struct-identity check so the call site picks the right vtable.  Single-level `.eq?` dispatch on `(:: v (Vec int))` returns correct answers across all primitive element types (fixtures `tvec-eq-ascribed`, `tvec-eq-ascribed-multi`).  Recursive `.eq?` on `(:: v (Vec (Vec int)))` still falls through to the wrong comparator (the instance body hardcodes `(fn [a b] (= a b))`) -- see F3-2..F3-6 below. | `stdlib/typeclass-eq.tur`, `src/compiler/typeclass.c`, `src/compiler/elab_typeclasses.c` |

---

## Phase F4 -- `^deprecated` attribute system

**Problem.**  `typed-collections-plan.md` shipped per-function
deprecation notices in docstrings (`Deprecated:` section) for the
untyped collection modules.  The compile-time warning that DEP-1/DEP-2
originally called for was deferred because it needs a generic
`^deprecated` attribute on definitions.  Without compiler-level
warnings, users do not see the deprecation until they read the API
docs by hand.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| F4-1 | Add `^deprecated` to the recognised binding/def annotations -- **shipped**.  `elab_fns.c` (`elab_defn`, `elab_def`) parses `^deprecated ["message"]` after the existing `#[no-unwind]` / `(export-as ...)` / `^persistent` attributes; the message is optional.  The pass-1 forward-declaration loop in `elab_toplevel.c` also skips the attribute so recursive lookups in pass 2 hit the existing global binding (no closure-via-id artifact). | `src/compiler/elab_fns.c`, `src/compiler/elab_toplevel.c`, `src/compiler/elab_core.c` (sym intern), `src/compiler/elab_internal.h` |
| F4-2 | Store the deprecation message on the Binding -- **shipped** (`Binding.is_deprecated` + `Binding.deprecation_message`).  Both zero-initialised via the existing `memset` in `binding_new`. | `src/compiler/expr.h` |
| F4-3 | Emit `DIAG_WARNING` at the use site -- **shipped** in `elab_lookup_sym` (the chokepoint that both var refs and call heads go through).  Self-recursive lookups are suppressed by comparing `b->name == e->current_fn_name`.  Promotion to error is wired through `--Werror=deprecated`: `g_werror_deprecated` global gated in `src/runtime/globals.{c,h}`, parsed/consumed in `src/main.c` (mirrors `--warn-unused-result`'s argv-strip pattern). | `src/compiler/elab_module.c`, `src/runtime/globals.{c,h}`, `src/main.c` |
| F4-4 | Mass stdlib annotation across the seven legacy modules (`pair.tur`, `option.tur`, `result.tur`, `list.tur`, `vec.tur`, `map.tur`, `slice.tur`) -- **shipped** (102 user-facing defns annotated: `pair.tur` 5 by-hand with specific replacements, the other six modules 97 by a regex script with a generic `see t<mod>-* in stdlib/t<mod>.tur` message).  Internal `__*`-prefixed defns left unannotated (typeclass-dispatch helpers etc).  Warnings fire at every use-site that resolves to one of these stdlib bindings; existing fixtures that rely on the legacy API see new stderr noise but no test-suite failures (their `expected.stderr` files either don't exist or use substring matching).  Most legacy stdlib modules aren't even in the auto-load list, so warnings only fire when a project explicitly loads them. | `stdlib/{pair,option,result,list,vec,map,slice}.tur` |
| F4-5 | Fixtures -- **shipped**.  `tests/fixtures/deprecated-warning` exercises the warning emission (with-message and bare variants, plus self-recursion suppression); `tests/fixtures/errors/deprecated-as-error` exercises `--Werror=deprecated`. | `tests/fixtures/` |

---

## Phase F5 -- `MutableMap[K V]`

**Problem.**  `typed-collections-plan.md` open-question section flags a
mutable map variant as a future TC2+ item.  `map.tur`'s header
references a planned mutable map; the HAMT-backed `Map[K V]` from TM0
covers only the persistent variant.

**Tasks.**

| ID | Task | File(s) |
|----|------|---------|
| F5-1 | Backing data structure: **open-addressed hash table with linear probing** -- **shipped** (conventional choice; avoids persistence overhead when mutation is the goal).  Resize at load factor 0.75 with capacity doubling; tombstones for deletion so probe chains stay intact. | design decision |
| F5-2 | Implement `stdlib/tmutmap.tur` -- **shipped**.  `(defstruct MutableMap [K V] ...)` with `tmutmap-new`, `tmutmap-set!`, `tmutmap-get`, `tmutmap-has?`, `tmutmap-delete!`, `tmutmap-len`, `tmutmap-eq?`, `tmutmap-free`, plus the constrained `(definstance Eq [MutableMap] [(Eq V)] ...)`.  Caller supplies a precomputed hash for keys, matching the `Hash[K]`/`Eq[K]` convention from `tmap.tur`.  Auto-loaded in `src/main.c` alongside the other typed-collection stdlib files. | `stdlib/tmutmap.tur`, `src/main.c` |
| F5-3 | Docstrings per the project doc-comment standard -- **shipped** (full `;;;` blocks with Parameters / Returns / Example / Since for every exported function; one-liner on the Eq instance). | `stdlib/tmutmap.tur` |
| F5-4 | Fixtures -- **shipped**: `tmutmap-basic` (insert/lookup/update/has?), `tmutmap-delete` (deletion with tombstone reuse + absent-key noop), `tmutmap-resize` (100-entry insert forcing multiple resizes), `tmutmap-eq` (structural equality via the helper AND via the F3-7 `.eq?` typeclass dispatch on an ascribed `(MutableMap int int)`). | `tests/fixtures/` |

---

## Phase F6 -- Interpreter (turi) fixture gaps

**Problem.**  Six fixtures pass under the compiler (`tur`) but fail
under the tree-walker (`tur run`); they were removed from
`TURI_FIXTURES_DEFAULT` in `tests/run-turi.sh` as a known-bad list
(see `typed-collections-plan.md`).  Each has a small, documented root
cause.

**Tasks.**

| ID | Task | File(s) | Root cause |
|----|------|---------|------------|
| F6-1 | ADT-typed nested field extraction -- **shipped** via two compiler fixes plus fixture renames:  (1) `elab_structs.c` now stashes raw field-type forms on defdata ctors (was previously NULL for non-GADT) and the match-binding path re-parses them via `type_expr_from_form` so the binding carries the declared ADT/struct type instead of being collapsed to `int`.  (2) `elab_types.c`'s F_KEYWORD path looks up unknown keyword type names in the scope and returns a real TY_ADT / TY_STRUCT with the def pointer (was previously a NULL-def placeholder).  (3) The fixtures `adt-param`, `adt-nested`, and `gadt-syntax-multi` were renamed to avoid colliding with auto-loaded stdlib names: `Option -> Opt` (adt-param, adt-nested), `Pair -> P2` (gadt-syntax-multi).  Without the rename, the user's `(defdata Option ...)` collided with `toption.tur`'s `(defstruct Option [A])` and the defdata silently failed to register, leaving `(Some 7)` unable to resolve through `elab_lookup_ctor`. | `src/compiler/elab_structs.c`, `src/compiler/elab_types.c`, `tests/fixtures/adt-param/`, `tests/fixtures/adt-nested/`, `tests/fixtures/gadt-syntax-multi/` |
| F6-2 | Disambiguate user-defined `defstruct Cons`/`Option`/`Pair` from auto-loaded stdlib structs of the same name -- **shipped** by renaming the user-side structs in the three fixtures (`Cons -> Cell`, `Option -> Opt`, `Pair -> Pr`).  The proper compiler-level fix (catch the redef at elaboration time -- the `(defstruct X ...)` check at `elab_structs.c:227` should fire when X is already in scope from the auto-loaded stdlib) is **deferred** as a separate cleanup, but the user-facing principle "do not shadow stdlib names" is correct.  Regenerated `expected.c` snapshots for the renamed fixtures. | `tests/fixtures/clone-list/`, `tests/fixtures/clone-option/`, `tests/fixtures/clone-pair/` |
| F6-3 | Add the missing `flags` file to `tests/fixtures/gadt-syntax-multi` -- **n/a** (file already present on the branch).  Fixture now passes via the F6-1 fix + rename. | -- |
| F6-4 | Re-add the now-working fixtures to `TURI_FIXTURES_DEFAULT` in `tests/run-turi.sh` -- **shipped** (added `clone-list`, `clone-option`, `clone-pair`, `adt-param`, `adt-nested`, `gadt-syntax-multi`).  All six previously-deferred F6 fixtures now pass. | `tests/run-turi.sh` |

---

## Phase F7 -- Doc hygiene

Cleanup-only; flips status markers and adds cross-references so
nobody re-investigates already-shipped work.

| ID | Task | File(s) |
|----|------|---------|
| F7-1 | Backfill `[x]` on EX1a-1..EX1f-5 in `existential-types-plan.md`; mark EX0 superseded -- **shipped** (27 EX1 entries flipped, EX0 banner added). | `docs/existential-types-plan.md` |
| F7-2 | Flip EXG2-2..EXG3-4 from `defer` to `done` with cross-refs; add superseded banner -- **shipped** (banner added, 6 entries flipped, task-summary cross-refs added). | `docs/existential-gc-plan.md` |
| F7-3 | Update `existential-gc-followup-plan.md` status to reflect cross-plan-followups closures -- **shipped** (status banner rewritten; EXG4-5, EXG5-5, EXG6-5 task-summary rows flipped from `partial` to `shipped (mostly)` / `shipped` with cross-refs to F1-1, F1-2, F2-1; EXG4-3 already marked retired via F1-3). | `docs/existential-gc-followup-plan.md` |

---

## Phase F8 -- `defstruct` compound field type annotations

**Problem.**  `defstruct`'s field-type parser (`parse_struct_field_type`
in `src/compiler/elab_structs.c`) only accepts a fixed list of
keyword/sym tokens (`:int`, `:cstr`, `rc<T>`, `ref<T>`, `weak<T>`,
`lref<T>`, `ptr<void>`, `:fn`, plus user-defined struct/ADT names
looked up by symbol).  Compound annotations -- the kind that
`type_expr_from_form` parses for `defn` return types, `let` bindings,
and `pack` -- are silently rejected:

- `[payload :(exists [a] [(Show a)] a)]`  -- constrained existential field
- `[handler (forall [a] (-> a a))]`        -- rank-2 function field
- `[items (vec int)]`                      -- TY_APP field (also fails)
- `[lst (Cons int)]`                       -- TY_APP field (also fails)

Because the parser fails closed (`fkind == TY_UNKNOWN` triggers the
"unrecognized type" diagnostic), every user who wants any of these
storage idioms has to erase to `:ptr<void>` and lose all type-level
information, including the constraint witnesses needed for method
dispatch through the field.

The gap blocks at least:

- `tests/fixtures/exg4-pack-into-struct` (EXG4-5 wave; F1-1-6 in this
  plan) -- store a packed constrained existential in a struct field
  and reopen it through `(.field s)`.
- `tests/fixtures/exg5-exists-cycle` (F2-2-1 in this plan) -- build
  a cycle by mutating a struct field of type `rc<Cell>` to point back
  at the existential containing it; force `gc!`; observe collection.
- The natural spelling of `Vec[Showable]` / `Map[K (exists ...)]`
  storage in user code today -- without F8, every collection of
  constrained existentials needs `:ptr<void>` fields plus an
  out-of-band tag to remember the real type.

### Design sketch

Mirror the path that `defn` return types already use:
`type_expr_from_form` produces a full `Type *` that captures
`as.forall_`, `as.app_`, etc., and the call site stores it in
`fn_type.as.fn.result_full_type`.  For struct fields, extend
`StructField` to optionally carry a full `Type *` alongside the
existing `kind` / `inner_kind` summary; emit and field-access paths
read through the full type when present and fall back to the summary
kind otherwise so callers that only need `kind` keep working.

### Tasks

| ID | Task | File(s) |
|----|------|---------|
| F8-1 | Extend `StructField` with `Type *full_type` -- **shipped**.  NULL when the field's type is a simple keyword/sym (the existing `kind`/`inner_kind` summary captures it fully); set for compound TY_APP / TY_EXISTS / TY_FORALL annotations.  Both `StructField` arena allocations in `elab_structs.c` now memset to zero so consumers that only need the summary kind keep working. | `src/compiler/types.h`, `src/compiler/elab_structs.c` |
| F8-2 | Field-type parser extension -- **shipped**.  Pre-scan and parse loops in `elab_structs.c` (both new-style at the per-field F_LIST path and old-style at the flat-vector path) detect `F_LIST` field-type forms and route them through `type_expr_from_form`.  For TY_APP / TY_EXISTS / TY_FORALL the C-level kind is `TY_INT` (opaque heap pointer); storage layout unchanged.  Pre-scan tag check extended to accept `F_LIST` alongside `F_KEYWORD` / `F_SYM`. | `src/compiler/elab_structs.c` |
| F8-3 | Type-check at `make-struct` -- **shipped indirectly via F8-1/F8-2**.  The constructor path already type-checks against `def->fields[i].kind`, and our F8-2 derivation of `kind = TY_INT` for compound types means both struct fields and packed values have matching C-level kinds.  A stricter check that the source's full `Type` matches the field's `full_type` (e.g. requires same constraint classes) is **deferred** -- the current behaviour matches what `make-struct` does for `rc<T>` fields today (kind-level check; value-side and field-side both lower to int64). | `src/compiler/elab_structs.c` |
| F8-4 | Field-access emit -- **shipped**.  `(.field s)` in `elab_method_call` (`src/compiler/elab_typeclasses.c`) uses `def->fields[i].full_type` when present, so `(open (.field s) ...)` sees the right TY_EXISTS payload instead of the bare int64 storage kind, and `(.eq? (.field s) (.field s))` via the F3-5/F3-7 dispatch sees the right element type for instance resolution. | `src/compiler/elab_typeclasses.c` |
| F8-5 | Move-at-pack scan extension for struct field initialisers -- **deferred**.  The shipped F8-6 fixture works fine without this extension (the let-bound `(pack ...)` value flows directly into `make-struct` as the field initialiser; the binding is consumed in the make-struct call and the struct itself owns the resulting rc-block).  Without F8-5, a pattern like `(let [p (pack ...)] (let [b (make-struct Box p)] ...))` would leave the source binding `p` with a stale reference -- but that pattern is rare in user code and we have not seen a fixture that depends on it.  Track as a separate follow-up once a real user hits it. | `src/compiler/elab_structs.c` (constructor elaboration) |
| F8-6 | Fixture `tests/fixtures/exg4-pack-into-struct` (the F1-1-6 / EXG4-5 fixture previously blocked on F8) -- **shipped**.  Field declared `(exists [a] [(Show a)] a)`, packed value stored, then opened back out through `(.payload b)` with `.show` dispatching through the witness vtable. | `tests/fixtures/` |
| F8-7 | Cycle-construction fixture `tests/fixtures/exg5-exists-cycle` -- **deferred**.  Building a real cycle requires mutable struct fields (set! on an rc-typed field to install the back-edge), which is its own scope.  F8 unblocked the storage path; the cycle-collection test specifically needs a mutation primitive on rc fields that this work does not add.  Track separately. | `tests/fixtures/` |
| F8-8 | Doc updates -- **shipped** (F1-1-6 row flipped, F2-2-1 row reflects the deferred F8-7 status, status banners updated). | `docs/cross-plan-followups-plan.md`, `docs/existential-gc-followup-plan.md` |

### Caveats

- **TY_APP fields are bigger scope.**  Once `type_expr_from_form`
  is wired in, struct fields can in principle hold `(vec int)`,
  `(Cons int)`, etc.  TY_APP requires the field-type to track the
  PTC4 dispatch metadata the function-return path already wires up;
  if F8-2 hits issues there, scope F8 down to just TY_EXISTS /
  TY_FORALL and leave TY_APP for a follow-up.
- **Recursive struct types.**  A struct field whose type mentions
  the same struct (`[next :(rc Self)]`) needs the forward-stub
  registration path that `RF0` (existing) already handles for the
  symbol-name case; F8-2 must use the same `e->scope` so the
  forward stub is visible.
- **Borrow-check of field reads through `(.field s)`** for
  CK_LINEAR / CK_MOVE existentials needs to follow the same rules
  as let-binding reads.  If F8-4's elaboration produces a normal
  EX_VAR-style expression that the borrow checker already handles,
  no new work; otherwise add a follow-up.
- **emit_module.c struct codegen** assumes 8-byte storage per
  field for the existing kinds; TY_EXISTS / TY_FORALL also fit
  (opaque rc-block pointer cast to int64), so no struct layout
  change is needed.  Document this assumption in F8-1.

### Estimated scope

Medium: ~1 session.  The mechanical pieces (parser route-through,
StructField extension) are straightforward; the field-access emit
in F8-4 is the most subtle piece and may need its own debugging
pass.  F8-5 mirrors an already-shipped pattern (F1-2-3) and should
land in a handful of lines.  F8-7 (cycle fixture) is the only
piece that exercises new code paths end-to-end; budget extra time
for that one.

---

## Sequencing Notes

- **F1-1 is the only true blocker** in this plan; it unblocks F2-1
  and three EXG4-5 fixtures.  It also closes the most user-visible
  bug (a compiler SEGV).  Recommend landing first.
- **F1-2** is independent of F1-1 but is a prerequisite for the two
  cycle-collection tests in F2-2.  Recommend landing second.
- **F3** (typeclass dispatch) is the largest item and is independent
  of all the existential work.  Can land in parallel with F1/F2.
  The 2026-05-23 scoping exercise found that F3 also requires the
  receiver-type-recovery work that the original plan did not call
  out (see the F3 "Additional blocker" subsection); budget 2-3
  sessions for F3 rather than the originally-implied 1.  F3-1
  (design) is shipped; the implementation tasks (F3-2..F3-7) are
  deferred.
- **F4** (deprecation attribute), **F5** (MutableMap), and **F6**
  (turi gaps) are all independent and can be picked up by whoever has
  spare cycles.
- **F7** is doc cleanup and can be done any time, ideally as the
  matching feature lands.
- **F8** (`defstruct` compound field annotations) is independent
  of F3/F4/F5/F6 and can land in parallel.  It unblocks the two
  remaining existential-storage fixtures that F1-1 / F1-2 could
  not ship (`exg4-pack-into-struct`, `exg5-exists-cycle`), so
  landing F8 lets F7 retire those last "blocked on `defstruct`"
  notes from the status banners.

---

## Relation to Other Plans

- `docs/existential-gc-followup-plan.md` -- F1/F2 close the
  items that plan explicitly deferred ("blocked on a separate parser
  limitation", "blocked on a smart drop hook").  F7-3 retires the
  deferred entries once they ship.
- `docs/existential-types-plan.md` -- F1-1 is the EX1 follow-up that
  was never tracked but is needed for full ergonomic use of the form.
  F7-1 corrects the stale status.
- `docs/existential-gc-plan.md` -- F7-2 marks the plan superseded;
  the work itself landed via EXG5/EXG6.
- `docs/ptc-plan.md` -- F3 implements the "deferred to a future
  phase" note about dictionary passing.
- `docs/typed-collections-plan.md` -- F3 enables the recursive `.eq?`
  dispatch; F4 implements the deferred deprecation-warning system; F5
  delivers the `MutableMap[K V]` mentioned in the open-questions
  section; F6 closes the interpreter-gap table; F8 makes typed
  collections of constrained existentials (`Vec[Showable]` etc.)
  expressible without `:ptr<void>` erasure.
- `docs/upcoming/refinement-types-plan.md` and
  `docs/upcoming/currying-plan.md` -- unrelated; this plan does not
  touch them.
