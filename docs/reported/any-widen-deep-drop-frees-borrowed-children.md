# A deep `any` drop freed children a live local still owned

**Severity:** was HIGH (a heap-use-after-free in plain Turmeric, silent without
a sanitizer); **fixed 2026-10-21**. What stays open is low: the boxes the fix
now declines to free leak instead.

**Status:** the three use-after-free/double-free paths are closed and pinned.
Open: the leak residue below.

Ported from the abandoned branch `claude/non-macos-windows-reports-6pz2pr`
(2026-09-18), which found and fixed all three but never merged. Main kept all
three holes in the meantime; a Saffron program built with the collector (the
default since 2026-09-28) hid them, plain Turmeric never did.

## Repro (before the fix)

```turmeric
(defdata Lst [] (Cons [hd : any tl : any]) (Nil))
(defn llen [xs : any] : int (match (cast xs Lst) (Cons h t) (+ 1 (llen t)) (Nil) 0))
(defn main [] : int
  (let [one (Cons (:: 1 any) (:: (Cons (:: 2 any) (:: (Nil) any)) any))]
    (println (llen (:: one any)))
    (println (llen (:: one any))))
  0)
```

```
SUMMARY: AddressSanitizer: heap-use-after-free ... in llen
```

## Root cause

`__tur_any_drop` is deep: it runs the payload's `drop_localowned_<T>` (the row
names it when `adt_def_has_localowned_glue` -- a recursive-self or `:any`
field) before freeing the box. A widen mints a fresh box but fills it with a
COPY of the payload's header, so the copy's children are still the
original's. Three rules read "the box is fresh" as "everything under it is
ours":

1. **`returns_fresh_any`** (`elab_fns.c`). A function whose tail is a widen
   mints a box per call -- but `(defn wrap [x : any] : any (:: (Cons x ...) any))`
   fills the box's `:any` field with the caller's value, and the consumer's
   deep drop walked into it.
2. **`any_expr_is_owned_temp`** (`elab_call.c`), the widen base case. The
   repro above: `(llen (:: one any))` stamps the widen for a drop at the call.
3. **The localowned scope drop** (`elab_forms.c`). In
   `(let [inner (Cons ...) outer (Cons (:: inner any) ...)] ...)`, `outer`'s
   glue frees `inner`'s spine through the copy and `inner`'s own scope drop
   frees it again. Nothing in the move discipline treated a stored widen as a
   hand-off.

## Fix

1 and 2 share one guard, `any_widen_payload_owns_droppable`: the drop is
licensed only when the payload owns nothing, i.e. exactly the types whose `any`
row names no glue, so the deep drop degenerates to the box free those rules
were written for. 3 adds `is_binding_widened_to_any` (the
`is_binding_consumed` walk with one more pattern: the binding widened and
passed to a call or struct literal whose result carries drop glue), searched
over the body and the sibling inits. All three can only decline a free.

Two companions from the same branch:

- **`tests/run-leak-check.sh` no longer lets `known-leak` excuse a non-leak.**
  `exitcode=23` is ASan's exit code for every error, so a marker keyed on the
  exit code excused a use-after-free as KNOWN. It now requires the summary to
  read `leaked in`. Without this the first fixture below would have stayed
  green on the unfixed compiler.
- **`box_uses_confined` checks a dynamic call's operands** instead of refusing
  on the presence of one. `lmap`'s `xs` read as retained because a sibling
  subexpression called `(f h)`. `any-widen-frame-box-past-dyn-call` went from
  2500 leaked allocations to none; `saffron-higher-order` from 880 B / 22
  allocations to 560 B / 14.

Pinned by `any-widen-deep-drop-plain-turmeric` (all three shapes, plain
Turmeric), `any-widen-fresh-payload-aliases` and `any-widen-of-a-borrowed-local`
(Saffron), `any-widen-frame-box-past-dyn-call` and its counter-case
`any-widen-frame-box-dyn-call-operand`. Each of the first three is a
use-after-free under `tests/run-leak-check.sh` on the unfixed compiler.

## Narrowed 2026-11-06: an explicit widen argument lives in the frame

`(llen (:: one any))` -- the `peek-it` shape -- leaks nothing now. The
frame-box rule (the widen's header copy goes in the caller's frame when the
callee keeps nothing of it and cannot suspend, `src/compiler/elab_call.c`) ran
only for an IMPLICIT widen, at a parameter that wanted `any`. An explicit
`(:: one any)` argument is already an `any`, so it went to the owned-temp rule
alone, which declines for exactly this payload. Under the same conditions it
is frame-boxed now, when the owned-temp rule declines. Fix direction 2's
"shallow drop" falls out: there is no box to drop.
`any-widen-deep-drop-plain-turmeric` went from 400 B in 10 allocations to 200 B
in 5; what is left there is `wrap-it` (its callee does keep the widen) and
`store-it` (shape 3).

Found on the way, pre-existing: widening a large by-value ADT PARAMETER
(passed as `const T *`) did not compile -- both arms of the by-value
widen copied the pointer (`expr_is_pbp_param`, `src/compiler/emit_expr.c`,
now dereferences). Pinned by `any-widen-explicit-arg-frame-box`
(leak-checked; the widened `one` is a parameter in `twice`, and is read again
after every call).

## Narrowed 2026-11-06: a fully fresh constructor tree is dropped deep

Fix direction 1 below. `any_widen_payload_deeply_fresh` (`src/compiler/elab_fns.c`)
admits the drop when the payload is a constructor call whose every argument is
a plain scalar or cstr, a widen (not frame-boxed) of one, or -- recursively --
another such constructor (directly, or under a widen). Both freshness rules ask
it beside `any_widen_payload_owns_droppable`: `any_expr_is_owned_temp` (so
`(llen (:: (Cons (:: i any) (:: (Nil) any)) any))` frees its tree at the call)
and `returns_fresh_any` (so a function whose tail widens such a tree is fresh).
A variable, a parameter, an opaque call or a closure anywhere in an owned field
still declines. Pinned by `any-widen-deep-fresh-tree-dropped` (leak-checked:
100 turns of an inline tree, a returned one and a cstr leaf; 2-3 boxes a turn
leaked before) and its must-keep twin `any-widen-deep-fresh-tree-kept` (a
variable child and a parameter child, each read after the consuming call;
`known-leak`). The residue below is unchanged: neither fixture there builds
its tree in one expression.

## Open: the leak residue

Declining the drop leaks the box (and, for shape 3, `inner`'s spine when
nothing else frees it). Measured: `any-widen-deep-drop-plain-turmeric` 400 B in
10 allocations, `any-widen-fresh-payload-aliases` 200 B in 5, both
`known-leak` against this report. A Saffron default build's collector reclaims
these; plain Turmeric and `--shared` do not.

Fix directions:

1. ~~**Deep freshness.**~~ Done 2026-11-06, above. Admit the drop when every owned child of the payload
   was itself minted in the same expression -- `(:: (Cons (:: 1 any) (:: (Nil) any)) any)`
   is fully fresh. Cannot help a constructor filled from a parameter or an
   opaque call's result.
2. **A shallow drop for a copied header.** A widen of a NAMED local could free
   only the box, leaving the children to the local's own drop. That needs the
   drop site to know which widen produced the box; today the row's glue is
   type-keyed, so the distinction would have to ride the call site.
