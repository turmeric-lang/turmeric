# effect-ref: an owning `ref<T>` across a `perform` (RESOLVED via the RC single-shot pass)

**STATUS: RESOLVED.** `effect-ref` DK-lowers -- both `sum-with-base` and `main`
(output `142`, zero eff=1).  The fix was much smaller than the CT-level
drop-at-last-use feared below: grant a `ref` the SAME single-shot-continuation
pass RC already has.

## The fix (grant a ref RC's single-shot pass; verified memory-safe by ASan)

`ref_dropped_before_control` (emit_cps_ir.c) returned false at every control op,
so a ref crossing a perform evicted -- the source comment claimed the "DK
teardown that frees a captured ref" substrate did not exist.  It DOES: granting
`ref_dropped_before_control` the same CT_PERFORM/CT_HANDLE/CT_RESET/CT_AWAIT ->
true pass that `owning_dropped_before_control` gives RC (flag-gated) makes the ref
captured into the single-shot continuation's env and freed there, exactly like an
RC handle.  The capture-admission gates (collect_caps single-shot vs
collect_caps_case multi-run) already prevent the multi-shot double-free, so the
soundness argument is identical to RC's.

**Memory-safety verification (this is drop analysis -- the important part):**
- effect-ref runs correct (`142`); under ASan the `ref<int>` is FREED -- the only
  leak is a 104-byte DK continuation NODE, and a ref-FREE variant
  (`(+ 100 (perform ...))`) leaks the IDENTICAL node, so the leak is the general
  perform-continuation pattern, NOT introduced by the ref capture.
- ASan sweep over ALL 26 ref-using fixtures: ZERO use-after-free / double-free /
  heap-overflow.
- Flag-off byte-identical; flag-on effect soundness sweep clean; full suite green
  (2203/0, Debug ASan build -- the compiler path is leak-checked).

The CT-level drop-at-last-use plan below is NOT needed -- the capture-into-
continuation path handles it.

## Original diagnosis (retained)

## The shape

```turmeric
(defeffect GetBase [] : int)
(defn sum-with-base [] : int
  (let [base (ref 100)]                       ; owning ref<int>, auto-drop at let exit
    (+ (deref base) (perform (GetBase)))))    ; deref in operand 0, perform in operand 1
(defn main [] : int
  (println (handle (sum-with-base) (GetBase [] k) (resume k 42))))
```

`base` is deref'd (its value copied out) in operand 0 of the `+`, then the
`perform` runs in operand 1.  In EVALUATION ORDER `base` is DEAD before the
perform -- it does not need to survive into the continuation.

## Root cause (drop-placement granularity)

`letraw_ok` rejects the `ref` because `ref_dropped_before_control`
(emit_cps_ir.c) does not find `base`'s `drop!` before the perform.  RC values get
a "captured into the single-shot continuation, dropped there" pass
(`owning_dropped_before_control` at ~1750), but a `ref` gets NO such pass -- the
DK teardown that frees a captured ref is P2/P3 substrate that does not exist yet
(pinned in the source comment ~1789).

O1-b (`plan_autodrop`, cps_ir.c ~2270) DOES hoist a ref's scope-exit auto-drop to
BEFORE a control op when the ref's live range does not cross it -- BUT it operates
at **do-item granularity**: `sum-with-base`'s let body is a single item, the `+`,
which BOTH references `base` (operand 0) AND contains the perform (operand 1).
`item_has_control(the +)` is true and `expr_refs_binding(the +, base)` is true, so
the coarse check says `base` crosses -> routes to the ref-reject.

The deref and the perform are in ONE expression, so:
1. A finer crossing check (base is referenced only in operand 0, evaluated BEFORE
   the perform in operand 1 -> does NOT cross) would correctly say non-crossing,
   BUT
2. The plan can only hoist the drop BEFORE the whole item (the `+`), which would
   drop `base` before its own `(deref base)` -> USE-AFTER-FREE.

So the drop must land at the **CT level**: after `base`'s last use is atomized
(the `deref` temp is bound) and BEFORE the emitted `CT_PERFORM`.  The do-item
plan cannot express that intra-expression position.

## Fix direction (CT-level drop-at-last-use before a control op)

Place the ref's auto-drop in the CT stream at the point after its last atomized
use and before the next `CT_PERFORM`/`CT_HANDLE`/... , when a live-range analysis
proves the ref is dead there.  This is drop-at-last-use in the CT IR, not a
do-item hoist.  Alternatively (heavier): add the missing P2/P3 substrate -- ref
capture + `free` teardown in the single-shot continuation frame -- so a ref gets
the same pass RC already has.

**MEMORY-SAFETY NOTE:** this is ownership/drop analysis -- under-detecting the
live range (dropping a still-live ref) is a use-after-free / double-free, not a
mere eviction.  The crossing default MUST stay "crosses"; any narrowing needs a
provably-sound eval-order liveness argument + the full ASan suite.  Do NOT rush.

## Verification recipe

- effect-ref DK-lowers (output `142`, zero eff=1); ASan clean (no
  leak/use-after-free on the ref).
- Flag-off byte-identical; FULL flag-on soundness sweep; full suite green under
  the Debug ASan build.

## Context

Stage E residual alongside `effect-nested` (value-position nested handle) and
`effect-capture-k` (by-reference mutable capture -- a set! writes the captured
continuation into an outer `^mut`, resumed after the handle exits; the DK has no
by-ref mut capture).
