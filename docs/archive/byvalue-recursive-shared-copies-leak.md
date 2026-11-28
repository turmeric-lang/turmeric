# A by-value recursive value held in a container never has its spine freed

**RESOLVED 2026-11-28 (archived).** Item 1 is fixed (clone at the borrow,
"Fixed 2026-11-28" at the end). Item 2 -- a container never frees its
elements' spines -- is the container ownership model rather than a hole in
this analysis, and it moved, with the same question about a fat-handle box a
`vec-push!` keeps, to
[container-elements-have-no-owner](../reported/container-elements-have-no-owner.md).

**Severity: low.** Leaks only; every shape that used to be a use-after-free
is closed. Filed while closing
[byvalue-recursive-adt-boxes-are-never-freed](byvalue-recursive-adt-boxes-are-never-freed.md);
**narrowed 2026-09-28** and **2026-11-28** -- see "What was fixed" below.

**Narrowed 2026-11-28: item 1, clone at the borrow.** A shared view handed to
a consuming parameter gets a deep copy of its own, so one borrowing call site
no longer costs every owning caller its discharge, and the view's owner is no
longer refused its free. Left: item 2, a container's elements. See "Fixed
2026-11-28" at the end.

## What is left

1. ~~**A consuming callee reached from a borrow discharges nothing, for every
   caller.**~~ Fixed 2026-11-28, below. Whether a parameter is owned is a whole-program fact (emit_core.c,
   "ownership PROVENANCE"): one call site passing a borrowed copy --
   `(tail2 xs)` with `xs : ^borrow` -- makes the parameter unowned everywhere,
   so the callee's owned callers leak what it would have freed.  The borrowing
   caller's own argument leaks too: `via-borrow` below hands its borrow to a
   callee that neither proves it keeps nothing nor returns only an alias, so
   the caller's local is not confined and is not freed.
2. **A container of by-value recursive values never frees its elements'
   spines.** This is the container ownership model rather than a hole in it: a
   compiled `Vec` local is not freed at scope exit at all (`vec-free` is the
   explicit release, and regions reclaim a non-escaping one), and `vec-free`
   frees the element slots, not what they point at.

Both were pinned by `tests/fixtures/byval-recursive-adt-shared-copy-not-freed`
(`known-leak`: 176 bytes in 7 allocations; the gate still fails on a
use-after-free). Item 1's 48 B is gone; the 128 B in 5 allocations left are
the fixture's `Vec`, its buffer and its element's spine.

```turmeric
(defn tail2 [xs : Lst] : Lst
  (match xs (Cons h t) t (Nil) (Nil)))
(defn via-borrow [^borrow xs : Lst] : int (llen (tail2 xs)))
```

## Fix directions

- For (1), done 2026-11-28: **clone at the borrow**. A `^borrow` (or global, or element) value
  passed to a CONSUMING parameter is a move out of something that keeps
  owning it -- Rust rejects it -- and a deep clone at that call site would keep
  the callee's parameter owned for every caller.  Needs a
  `clone_localowned_<T>` glue beside `drop_localowned_<T>` (recursive fields
  copied box by box, rc fields incremented, `any` fields deep-copied), which
  does not exist yet.  Per-call-site ownership (emitting a non-discharging
  twin of the callee for borrowed callers) is the alternative, and costs a
  variant propagated through the call graph.
- For (2): element drop glue for a `Vec` of a by-value recursive type, once
  `Vec` locals themselves have an owner.

## What was fixed (2026-09-28)

- **An rc-field struct copied out of a shared view was a live use-after-free**
  (the "same hole in another type family" this report used to end on, and it
  was wider than filed): a copy of a `^borrow` returned by value, a global
  returned by value, a `vec-get` element and a match binder of a borrowed ADT
  each had a let-local decrement a count it never took (ASan, all four).  A
  SHARED VIEW -- a `^borrow`, a global, a container element, a binder of one --
  is now cloned where it becomes an owner (a let-local, a function result, a
  `return`): the clone takes +1 per rc field (elab_forms.c,
  `elab_own_byval_copy`).  A `ref` field has no count to take: a let-local that
  may hold a borrowed one leaves it to its owner, and a RESULT that would is
  TUR-E0108 ("cannot move out of a borrow").  `byval-rc-struct-shared-copy`,
  `errors/byval-ref-field-returned-from-borrow`.
- **The source of a copy lent to a `^borrow` callee is freed** (was item 1).
  A `^borrow` recursive-ADT parameter kept nowhere but the result gets a
  result-alias bit (`resalias_param_mask`); the alias walk tracks a local bound
  to such a call as an alias of its argument instead of calling it a hand-off,
  and a walk rooted at a `^borrow` tracks every local bound into the root (none
  of them can be an owner).  `(let [w (id-b zs)] (llen w))` now frees `zs`.
- **A fresh spine lent to a non-retaining callee is freed after the call.**
  `(llen (build 3))` leaked the whole temporary on every call -- no owner at
  all -- and was not in this report; it is the commonest shape of the family.
  The spill temp joins the pending-drop queue that already frees a lent Option
  box.  `byval-recursive-adt-lent-temporary-freed` (300 allocations -> 0).
- The leak gate now builds with the runtime compiled from source
  (`TUR_RUNTIME=source`), so a use-after-free INSIDE the rc runtime is
  instrumented: with the prebuilt archive `rc_strong_decrement` ran
  uninstrumented and the rc-field double decrement passed the gate.

## Fixed 2026-11-28: clone at the borrow (item 1)

A SHARED VIEW of a by-value recursive ADT -- a `^borrow`, a global, a
container element, a binder of one (`elab_expr_is_shared_view`) -- handed to a
parameter that consumes it is now a deep copy. The elaborator rewrites the
argument to `(let [__shs v] <EX_BYVAL_CLONE __shs>)`
(`elab_clone_shared_spine`, `src/compiler/elab_forms.c`) when the callee is a
top-level, statically dispatched function whose parameter is declared at
that ADT, by value, and not proven non-retaining (`call_arg_needs_spine_clone`,
`src/compiler/elab_call.c`). The `let` hands the view's representation to the
let-init machinery, so the clone node only ever names a plain local.

- **The copy glue.** `clone_localowned_<T>` / `clone_glue_<T>`
  (`emit_adt_byval_drop_glue`, `src/compiler/emit_module.c`) mirror
  `drop_localowned_<T>` field for field: an `rc` takes a count, a `weak` a
  weak count, a box of the type itself is copied box by box from the
  constructor's allocator, and every other field is copied as it is -- the
  fields the drop leaves alone. A type with a field there is no copy rule for
  (`ref`, `any`, a boxed fn field, a box of another owning aggregate) is
  refused by `adt_byval_spine_clonable` (`src/compiler/types.c`) and keeps the
  old answer.
- **Ownership.** The provenance (`own_expr`, `src/compiler/emit_core.c`)
  calls an `EX_BYVAL_CLONE` owned, so the callee's parameter stays owned for
  every caller and discharges. The strict confinement walk
  (`box_uses_confined`) and the escape walk read the clone's operand as a
  read, so the view's owner frees its spine at scope exit -- `main`'s `ys` in
  the fixture above.
- **Not a move.** A view the call copies is read, not moved, so the move
  checker leaves it usable: a global handed to a consuming callee used to be
  marked moved for the rest of the program.
- **Forward and recursive callees.** A callee not elaborated yet has only a
  placeholder signature (its parameter kinds read `int`) and no inferred
  mask, so the argument is recorded (`Elab.pending_spine_clones`) and judged
  after Pass 2 (`elab_resolve_pending_spine_clones`). Its copy still keeps the
  callee's parameter owned. Later the same day Pass 2 began elaborating a
  defn with a by-value recursive ADT parameter before its callers, as it does
  a generic one (`fgo_ann_is_rec_adt`, `src/compiler/elab_toplevel.c`), so a
  forward callee is inferred by the time its caller is elaborated -- which
  also fixed an owned local LENT to a forward callee (it was moved, and
  nobody freed it) and a borrow handed to one (an escape);
  `tests/fixtures/byval-recursive-adt-forward-callee-lent`. What a cycle of
  such functions cannot be ordered around is
  [byval-adt-lend-across-mutual-recursion](../reported/byval-adt-lend-across-mutual-recursion.md).
- **Found on the way.** A `^borrow` handed to a result-alias parameter
  (`resalias_param_mask`) whose call returns a non-pointer scalar read as an
  escape outside a print: `(+ acc (via-borrow ys))` kept `ys`'s spine. Such a
  result cannot be the alias, so the walk admits it now.

The copy is O(n) in the spine: a consuming callee called on a borrow now pays
for the copy where it used to share the spine and leak every owning caller's.
Rust makes the same call explicit (`.clone()`); here it is implicit, so a hot
loop that hands a long borrowed list to a consuming function pays per call.

Pinned by `tests/fixtures/byval-recursive-adt-borrow-cloned-for-consumer`
(leak-checked: a `^borrow`, a global, a binder of a borrow, an `rc`-field
spine, a forward consuming callee, beside owning callers of the same
callees, 100 turns of each).
