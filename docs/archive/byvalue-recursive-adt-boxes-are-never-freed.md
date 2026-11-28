# A by-value recursive ADT leaks one box per link

> **RESOLVED 2026-09-28.** The last open shape -- a callee that CONSUMES the
> value and returns part of it -- is discharged at the callee, and closing it
> exposed a use-after-free in the 2026-09-07 scope-exit drop, fixed here too.
>
> **The own half** (direction 1's other half, `emit_core.c` +
> both ADT match paths in `emit_expr.c`). The report's worry was right: one
> body compiled once cannot know per call site whether its parameter is owned.
> It does not have to -- ownership is made a per-callee fact: a parameter is
> OWNED when every static call site passes a value it owns, the function's
> address is never taken, it is not exported (under separate compilation),
> not an instance method, not a C export, and not proven non-retaining (those
> are lent). When such a parameter's only use is a `match` on the body's spine
> (evaluated once per call), each arm, after its value is computed, frees the
> spine boxes it did not pass on: a recursive-field binder moved out exactly
> once (returned, or handed to a constructor or a consuming callee) frees the
> box its contents were copied from (SHALLOW, `tur_region_free`); a binder
> unused, or only lent (the strict alias walk), frees its whole sub-spine
> (DEEP, `drop_glue_<T>`). The parameter's own storage is the caller's and is
> never touched. Declined, as a leak: an arm emitted on the tail path (it
> returns / jumps first), a move INTO a tail call (a free after it would cost
> the C compiler the jump a recursive walker relies on), guarded arms, and any
> binder use the walk cannot place. `TUR_NO_OWN_DISCHARGE=1` turns it off for
> A/B. The report's repro -- `(let [zs (Cons 7 (Cons 8 (Nil))) ws (tail zs)]
> (llen ws))` -- is leak-clean; `byval-recursive-adt-consumed-by-callee` pins
> it with `first-only` (DEEP), `incr` (a consuming call inside a
> constructor), a two-field tree (`left`: SHALLOW + DEEP) and chained
> temporaries: 344 bytes / 13 allocations -> clean.
>
> **The use-after-free it exposed.** "Owned" needs a real answer, and the move
> checker does not give one: a by-value recursive value can be COPIED out of
> a `^borrow` parameter (`(defn id-b [^borrow x : Lst] : Lst x)`), a global,
> or a container element (`vec-get`), and the copy shares its boxes with its
> source. The scope-exit drop freed such a copy's spine under its owner --
> ASan heap-use-after-free for all three, on `main` before this change. So both
> consumers now ask a whole-program ownership PROVENANCE analysis: a value is
> owned when it is a constructor (recursive-type args owned), a moved owned
> local / binder / parameter, or the result of a user function whose every
> return value is owned (greatest fixed point with the parameter facts; the
> walk enumerates operands through the shared `cps_visit_children`). Those
> copies are refused and leak instead -- pinned by
> `byval-recursive-adt-shared-copy-not-freed` (`known-leak`, which still fails
> on a use-after-free) and filed as
> [byvalue-recursive-shared-copies-leak](byvalue-recursive-shared-copies-leak.md),
> along with the same hole for an rc-field struct copied out of a `^borrow`,
> which this analysis does not cover.
>
> Suite 3331/0, no snapshot moved; leak-check 108/0 with the new fixture the
> only new known-open row. `:copy` recursive ADTs stay the documented regions
> contract (Residue 2 below); gc-guide "Known gaps" is updated.

**Severity: low-medium.** One `malloc` per link of a self-recursive by-value
`defdata`, never freed.

**PARTIALLY FIXED 2026-09-07; NARROWED AGAIN 2026-09-19.** A non-escaping
local's spine is freed at scope exit (2026-09-07), and so is a local **lent to
a callee proven not to retain it** (2026-09-19, the common shape -- `(println
(llen xs))` is clean). What remains open is one shape: a callee that CONSUMES
the value and returns part of it. `:copy` recursive ADTs are now a documented
contract (regions), not a gap. Details under "Residue 1" and "Residue 2".

Split out of
[saffron-any-return-defeats-the-frame-box-rule](saffron-any-return-defeats-the-frame-box-rule.md).

## Repro -- no `any` anywhere, plain Turmeric

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))
(defn main [] : int
  (let [xs (Cons 1 (Cons 2 (Cons 3 (Nil))))] (println (llen xs)))
  0)
```

| cells | before |
|---|---|
| 3 | `72 byte(s) leaked in 3 allocation(s)` |
| 5 | `120 byte(s) leaked in 5 allocation(s)` |

Exactly one box per link, linear. Each is the heap copy of the recursive `tl`
field: a by-value product cannot ride the int64 carrier and cannot contain
itself inline, so the field slot holds a pointer to a heap copy.

## Root cause

`AdtDef.needs_drop_glue` is set when a constructor has an `rc`/`ref`/`weak`
field. A self-recursive by-value field is owning in the allocation sense (the
parent's slot is the only pointer to that box) but is not one of those kinds, so
no drop glue was emitted and nothing ever freed the chain.

## What was fixed

A DIRECT self-reference (`tl : Lst`, not `(Vec Lst)`) now points the field's
`drop_inner_def` at its own def, which makes the existing by-value drop glue
recursive -- exactly the walk a spine needs -- and a non-escaping local frees
that spine at scope exit via `drop_recspine_<T>(&xs)`, the twin of the
boxed-fn-field drop beside it. `tests/fixtures/byval-recursive-adt-spine-drop`
pins it under the leak harness.

**`:copy` is the soundness line, and it was measured rather than assumed.** Drop
glue makes a type move-only, and that move discipline is what guarantees the
single owner the free depends on:

```turmeric
(let [t (Cons 3 (Nil))  a (Cons 1 t)  b (Cons 2 t)] ...)
```

is already `TUR-E0201: cannot copy unique value 't'`. Under `:copy` the same
program compiles, and the emitted C shows `*__t185 = t` and `*__t187 = t` --
two boxes carrying the SAME tail pointer -- so a per-chain free would free it
twice. `:copy` recursive ADTs therefore keep the leak.

Flipping the flag alone changed nothing across the corpus (2861 passed / 0
failed, no codegen snapshot moved -- none of the 148 snapshot fixtures has a
self-recursive `defdata`).

## Residue 1 -- a local handed to a callee (the common shape)

**FIXED 2026-09-19 for a non-retaining callee, by direction 1 below -- the
borrow/own distinction, at the CALL rather than in the callee.** The callee is
asked whether its body retains the parameter, with the same inference that
already answers it for `any` / `cstr` / Option-Result parameters
(`nonretain_ptr_param_mask`, elab_fns.c), run in a new alias-aware STRICT mode
of the walk (`localowned_param_is_nonretaining`, emit_core.c): every match
binder and field read rooted at the parameter is tracked as an alias of it; an
alias reaching a store, a return, a let, a closure, or a callee not itself
proven non-retaining is retention; a non-pointer scalar binder (`h : int`) is
not tracked; and only a non-pointer scalar RESULT is admitted, since an
aggregate result could carry a sub-spine out. The self-call is the same
greatest fixed point the fn-param mask uses -- `llen` is assumed non-retaining
for its own recursive hand-off of `t`, and the assumption survives only if
nothing else in the body contradicts it. A caller passing a local to such a
parameter then LENDS it (`binding_mark_lent`): the static answer is unchanged
-- the binding is poisoned, a second use is still TUR-E0005/E0201 -- and the
caller's scope-exit drop fires (`Binding.lent_to_nonretaining` vs the sticky
`moved_owning`, elab_forms.c). Pinned by
`tests/fixtures/byval-recursive-adt-lent-to-callee` under the leak harness:
`llen`, `lsum` (an int binder beside the recursive one), a chained lend, a
consuming control, and the inline-match binder lent onward. Retaining callees
measured refused (no caller-side drop emitted): a match binder stored into a
global, captured by a closure, or let-bound; a callee returning the ADT.

Refusals worth knowing: a callee that writes a `^mut` global is refused by
the effect-free guard the frame-box rule also carries (its row is not empty),
not by the walk. And the emitted C has always passed a by-value ADT argument
by pointer (`llen(&xs)`); the lend changes nothing about the call, only about
who frees.

**Also closed 2026-09-19 (a soundness hole in the 2026-09-07 rule, not a
leak):** the scope-exit drop of a LOCAL consulted only the local's own moved /
consumed state, so a match binder captured by a closure or stored into a
global outlived the freed spine (measured once
fn-cell-set-with-capturing-closure-segfaults made the store compile:
heap-use-after-free). The drop now requires the same alias-aware walk
(`localowned_binding_is_confined`); pinned by
`tests/fixtures/closure-retains-match-binder-of-dropped-local`.

**Still open: a callee that consumes and returns PART of the spine.**
`(defn tail [xs : Lst] : Lst (match xs (Cons h t) t (Nil) (Nil)))` is
correctly refused (aggregate result), so the caller moves `xs` into it for
real; `tail` hands back the sub-spine and the box above it has no owner.
Measured: `(let [zs (Cons 7 (Cons 8 (Nil))) ws (tail zs)] (llen ws))` leaks
exactly one box -- the head -- where before this change it leaked all of them.
Discharging that needs the callee to know its parameter is OWNED (free the
boxes it does not return) rather than borrowed, i.e. direction 1's other
half, or direction 2.

The original analysis, kept because it is what shaped the fix:

`(println (llen xs))` marks `xs` moved, so no drop fires and the spine leaks
exactly as before. That is correct as far as it goes -- ownership went to `llen`
-- but nothing discharges it there, and completing the move discipline is
harder than it looks:

A pattern-match binder ALIASES the parent's spine. `(match xs (Cons h t) ...)`
gives `t` a pointer into `xs`'s box chain, and `llen` passes it to its own
recursive call. If a callee freed its by-value ADT parameter at scope exit, that
recursive call would free a sub-chain the outer frame also owns -- a double free,
not a leak. So parameter-side discharge needs to distinguish an owned argument
from a borrowed interior pointer, which the current move tracking does not.

Measured coverage as it stood on 2026-09-07: a local consumed by an inline
`match` or field read within its own scope is freed (the fixture); a local
passed to a helper is not. (As of 2026-09-19 it is, when the helper is proven
non-retaining -- above.)

## Residue 2 -- `:copy`, where regions already answer

**Direction 3 taken 2026-09-19:** this is now a documented contract in
`docs/guides/gc-guide.md` ("Known gaps"): a `:copy` recursive ADT is never
freed per value, because drop glue would make it move-only and the move
discipline is the single-owner guarantee a per-chain free depends on; build
such structures inside `with-region`, where the spine is reclaimed on rewind.

Most recursive types in the stdlib are `:copy` -- `Term`, `Subst` and `Stream`
in `logic.tur` (the workload `docs/archive/regions-plan.md` was priced on),
`Regex`, `RxCls`, `RxPos` -- so this residue is the larger one by usage.

It already has a working answer: inside a `with-region` bracket the whole spine
is reclaimed on rewind. Verified -- the 3-cell repro above wrapped in
`(with-region (fn [] : int ...))` reports zero leaks. R4 of the regions plan
routed this exact allocation site (`emit_expr.c`'s recursive-ctor-field box) for
that purpose.

## What is NOT this bug

`tests/fixtures/saffron-higher-order` was originally marked against this report.
That was wrong, and measuring the emitted C is what showed it: its `Lst` is
`(Cons [hd : any tl : any])`, so the tail is an `any` box from
`elab_coerce_to_any`'s by-value widen, not the recursive-carrier box at all --
11 widen sites, ZERO recursive-carrier sites. Same family (a box inside a
structure with no owner), different producer, different fix. Filed separately as
[any-widen-stored-in-an-adt-field-has-no-owner](../archive/any-widen-stored-in-an-adt-field-has-no-owner.md).

## Fix directions for the residue

1. ~~**Parameter-side discharge**, with a borrow/own distinction for by-value ADT
   arguments.~~ **DONE 2026-09-19 for the borrow half** (the mask now admits a
   by-value recursive ADT parameter, with interior pointers modelled as
   aliases). The OWN half -- a consuming callee freeing the boxes it does not
   return -- is what remains; it needs the callee to be compiled knowing the
   parameter is owned, which one body compiled once cannot be conditional on
   per call site, so it would be a per-callee fact (the parameter is owned at
   EVERY call because the callee is never proven non-retaining) plus a
   partial-spine free in the callee's arms.
2. **Refcount the link.** Settles the remaining shape, including `:copy`, by
   construction. Heavier, and it prices every construction.
3. ~~**Leave `:copy` to regions, and say so in the guide.**~~ **DONE
   2026-09-19** -- gc-guide "Known gaps".
