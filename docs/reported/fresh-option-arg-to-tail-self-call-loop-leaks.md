# A fresh `(Option Struct)` passed to a TAIL self-call leaks a box per iteration

**Severity: low (leak; grows with the loop's trip count).** The tail-position
sibling of the archived
[fresh-option-arg-to-self-call-leaks](../archive/fresh-option-arg-to-self-call-leaks.md):
that report's non-tail shape is fixed, this one is not. Found 2026-10-21 while
fixing it; it reproduces byte for byte on the compiler before that fix, so it
predates it.

## Repro

```turmeric
(defstruct Id :copy [ix : int])
(defn nxt [i : int] : (Option Id) (if (>= i 3) (none) (some (Id i))))
(defn walk-acc [o : (Option Id) acc : int] : int
  (match o
    (Some id) (walk-acc (nxt (+ (.ix id) 1)) (+ acc (.ix id)))
    (None)    acc))
(defn main [] : int (println (walk-acc (nxt 0) 100)) 0)
```

Built with `TUR_CC_FLAGS="-O1 -g -fsanitize=address"` and run with
`ASAN_OPTIONS=detect_leaks=1`: prints `103`, then
`16 byte(s) leaked in 2 allocation(s)` from
`some__spec__tur_adt_Option__Id_tur_adt_Id` -- the boxes of `(nxt 1)` and
`(nxt 2)`. `main`'s `(nxt 0)` is freed after the call returns.

## Root cause

The self-call is in tail position, so `tco_mark` (`src/compiler/emit_fns.c`)
lowers it to a back edge: `o = __t214; acc = __t215; goto __tur_tailcall;`.
The sum-box drop-after stamp (`elab_stamp_self_call_sum_drops`, and the
outside-caller stamp in `elab_call.c`) frees a fresh argument once the
consuming call returns, and a back edge has no "after": the new box becomes
the next iteration's `o`, and when the loop reassigns `o` again, the old box
is dropped on the floor. The emitter correctly frees nothing at the back edge
(the box is still live there), so this is a missing owner, not a wrong free.

## Fix directions

1. Give the loop the box: at a back edge whose argument for a
   non-retained sum parameter is a fresh box, free the parameter's current box
   before the reassignment -- except on the first iteration, whose box the
   caller owns (and frees after the call, when stamped). A per-parameter
   "owned by the loop" flag, cleared at entry and set at each such back edge,
   with a free at every `return` while it is set, is one shape. The mask's
   result gate (a non-pointer scalar result) already guarantees nothing
   returned points into the box.
2. Pin it with a `requires.leak-check` fixture beside
   `option-arg-to-self-call-freed` (which covers the non-tail shapes).
