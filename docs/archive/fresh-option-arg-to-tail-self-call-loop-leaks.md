# A fresh `(Option Struct)` passed to a TAIL self-call leaks a box per iteration

**RESOLVED 2026-10-22**; see Resolution at the end.

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

## Resolution (2026-10-22)

Fix direction 1, in the emitter (`src/compiler/emit_fns.c`). Before the loop
head, `tco_owned_params` finds the parameters some self tail call hands a FRESH
box the function does not keep -- the argument passes the same test the non-tail
drop-after stamp does (`emit_call_owes_sum_drop`, and the parameter's bit in
`nonretain_sum_param_mask`), the parameter holds a box this can free (the int64
carrier, or a by-value monomorph whose live arm boxes a value-struct payload, as
here: `emit_sum_box_drop_kind`), and the function's result is a non-pointer
scalar. Each gets an `int __tur_lown_<i> = 0;` flag. Then:

- **a back edge** whose argument is fresh frees the old box if the flag is set,
  reassigns, and sets the flag; one that passes the parameter on unchanged
  keeps both; any other argument clears the flag without a free, since it may
  alias the old box (`(let [p o] (walk p ...))`) -- the old leak, never a wrong
  free. The fresh argument's queued drop-after-consumer entry is discarded
  (`emit_pending_drops_discard`): the back edge is not its consumer.
- **every `return`** (value, void and explicit) frees the box while the flag is
  set. The free rides `any_scope_drops`, which the return paths already fire
  after hoisting the value; the back edge skips those entries
  (`tco_own_lo`/`tco_own_hi`).

The first turn's box is the caller's, so the flag starts clear and the caller's
own free after the call is unchanged. `option-arg-to-tail-self-call-freed`
(`requires.leak-check`) pins the repro and a two-field payload.

Not covered, and not this report's shape: a loop that passes the parameter on
UNCHANGED on some turns (`(walk-hold o (- hold 1) ...)`). The sum mask's
inference reads that self hand-off as an escape (it is not a greatest fixed
point, unlike the fn-param mask), so the parameter is not non-retaining and its
fresh boxes are not stamped -- they leak as before, tail or not. The back edge
already handles the unchanged argument (box and flag stay) for when the
inference learns it.
