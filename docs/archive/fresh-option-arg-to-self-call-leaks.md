# A fresh `(Option Struct)` passed to a self-recursive call leaks its box

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by
`option-arg-to-self-call-freed` (leak-checked). The tail-call sibling is filed
separately as
[fresh-option-arg-to-tail-self-call-loop-leaks](fresh-option-arg-to-tail-self-call-loop-leaks.md).

**Severity: low (leak; grows with recursion depth).** When a function calls
itself with a freshly made `(Option S)` as an argument, inside a `match` arm,
and `S` is a struct, the Option's heap box is never freed. Binding the
argument in a `let` first frees it. So does a non-recursive call with the same
argument.

Found 2026-10-16 walking the CommonMark block tree (markdown-library-plan M1):
a recursive walk over `first-child` / `next-sibling` Options leaked one box
per block. The library exposes `markdown/children`, a `Vec`, instead.

This is a narrower case of
[carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md),
which lists the consumers that do own a fresh Option box. This consumer, a
self-recursive callee's argument, is not on that list.

## Repro

```turmeric
(defstruct Id :copy [ix : int])
(defn nxt [i : int] : (Option Id) (if (>= i 3) (none) (some (Id i))))
(defn walk [o : (Option Id)] : int
  (match o
    (Some id) (+ 1 (walk (nxt (+ (.ix id) 1))))
    (None) 0))
(defn main [] : int (do (println (walk (nxt 0))) 0))
```

Built with `-fsanitize=address` and run with leak detection:

```
SUMMARY: AddressSanitizer: 16 byte(s) leaked in 2 allocation(s).
    #1 in some__spec__tur_adt_Option__Id_tur_adt_Id
```

The two boxes are the ones for `(nxt 1)` and `(nxt 2)`. The outer
`(walk (nxt 0))` in `main` is freed. Writing the arm as
`(let [n (nxt (+ (.ix id) 1))] (+ 1 (walk n)))` frees all three.

## Root cause (suspected)

A fresh box passed to a callee is freed after the call only when the callee's
inferred mask says it does not retain the argument (the drop-after stamp
described in the parent report). For a self-recursive callee that mask is not
known when the self-call is elaborated, so this call site gets no stamp.

## Fix directions

1. **Stamp the self-call too.** Treat a self-recursive call's argument the way
   a let-bound one is treated, freeing it at the scope exit of the arm. Or
   compute the mask before elaborating the body's self-calls, with
   "non-retaining" as the assumption the fixpoint refines.
2. **Guard it with a fixture:** a leak-checked recursive walk over `Option`
   of a struct, in direct and `let`-bound argument forms.

## Resolution (2026-10-21)

The suspected cause was right: the stamp keys on the callee's
`nonretain_sum_param_mask`, and a self-call is elaborated inside the very body
that mask is inferred from, while it is still zero. Fix direction 1, second
half: once `elab_infer_nonretain_masks` has run for a `defn`,
`elab_stamp_self_call_sum_drops` (`src/compiler/elab_call.c`, called from the
defn elaboration in `elab_fns.c`) walks the body for calls whose callee is the
function itself and stamps each fresh sum-box argument in a surviving mask
slot, by the same rule (and through the same helper, `stamp_fresh_sum_arg`) as
an outside caller. It is sound for the same reason the outside stamp is: the
mask's inference reads every self-call hand-off of a parameter as a possible
escape, so a surviving bit means the body neither keeps the box nor returns
anything that points into it. The walk visits the node kinds
`expr_subtree_has_inline_c` models, which is every kind a body with a
non-zero mask can hold; an unvisited kind would only leave a call unstamped.

The repro above now runs leak-free under LeakSanitizer and still prints 3.
`option-arg-to-self-call-freed` (`requires.leak-check`) pins the report's
shape, the let-bound form, a walk that reads the arm's payload after its
self-call returns, and a non-self caller in tail position. Snapshots are
unchanged (157), the full `run.sh` suite passes (3968), and so do the 235
leak-checked fixtures.

**Not covered: a TAIL self-call.** There the emitter lowers the call to a
back edge (`o = new; goto`), which has no "after" to free in, so the box
becomes the next iteration's parameter and is dropped when the loop reassigns
it. The emitter correctly frees nothing there; the leak predates this fix
(same 16 bytes before and after). Filed as
[fresh-option-arg-to-tail-self-call-loop-leaks](fresh-option-arg-to-tail-self-call-loop-leaks.md).
