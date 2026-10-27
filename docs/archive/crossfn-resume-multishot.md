# Cross-function resume: multi-shot receiver returns a wrong value

**Status: RESOLVED (cps-backend-n6).** The auto-desugar now reflavors the
receiver's `cont` param to `multishot-effect-cont` (CONT_EFFECT + CK_MULTISHOT)
and installs a `^multishot` __Shift handler, so a receiver may resume `k` any
number of times.  The opening example runs `23`; a triple-resume runs `306`
(fixture `shift-crossfn-resume-works`); the flavor is pinned directly by
`multishot-effect-cont-kv-sugar`.  `direct == cps == turi`.  Both fix directions
below were applied, plus a third that surfaced during implementation:
`expr_has_multishot_handler` (emit_core.c) gained `EX_RESET`/`EX_CLONEABLE_RESET`
cases so it descends into a reset body and finds the synthesized multishot
handler -- without it the cloneable-cont preamble was not emitted and the build
failed to link.  Original report retained below.

**Severity: low (known limitation; single-resume is the supported target).**

## Summary

The shift/reset cross-function resume auto-desugar
(`cps-backend-n6-crossfn-resume-desugar-plan`) supports a **single-resume**
receiver. A receiver that invokes the delimited continuation `k` **more than
once** silently returns a wrong value instead of the multi-shot sum, because the
synthesized `__Shift` handler continuation is one-shot (`CK_UNIQUE`) and the
receiver's `(k v)` lowers to the one-shot effect-resume path
(`tur_effect_cont_resume`), not the cloneable-snapshot path.

## Repro

```turmeric
;; receiver resumes k twice: (k 1) + (k 2)
(defn inner [] : int (shift (fn [k : cont] : int (+ (k 1) (k 2))) 0))
(defn outer [] : int (reset (+ 10 (inner))))    ; k = (+ 10 [])
(defn main [] : int (println (outer)) 0)        ; want (10+1)+(10+2)=23
```

Observed: `22` (both `direct` and `turi`, so they agree — this is a desugar
limitation, not a backend divergence). Want: `23`.

`22` arises because the second `(k 2)` resumes an already-consumed one-shot
continuation and reuses the first resume's state, so the second resume yields
`11` again (`11 + 11 = 22`) instead of `12`.

Single-resume is correct: `(fn [k : cont] (k 1))` gives `11`
(fixture `shift-crossfn-resume-works`).

## Root cause / fix directions

Two coordinated changes are needed (both under `cps-backend-n6`):

1. `wrap_reset_body_with_shift_handler` (`src/compiler/elab_effects.c`) builds the
   `__Shift` handler case with `cont_kind = CK_UNIQUE`. For multi-shot it must be
   `CK_MULTISHOT`, so the captured continuation is snapshot-capable (mirrors the
   `multishot-handler` fixture, which resumes `^multishot k` twice and sums 30).
2. The receiver's `effect-cont` `(k v)` (the `CONT_EFFECT` branch in
   `elab_call.c`) emits `EX_RESUME` with an `int64`-carrier `EX_VAR`, which takes
   the `TY_INT` one-shot path in `emit_effects_resume`. For multi-shot it must
   carry `CK_MULTISHOT` so the resume takes the
   `tur_cloneable_cont_resume(tur_continuation_snapshot(k), v)` snapshot path.

The wrinkle: `CONT_EFFECT` must stay **one-shot by default** — the hand-written
slice-A fixture `effect-cont-kv-sugar` pairs a `CK_UNIQUE` handler with an
`effect-cont` receiver and must not snapshot. So multi-shot needs a distinct
signal (e.g. a `multishot-effect-cont` flavor, or threading the handler's
`cont_kind` to the receiver's reflavor) rather than making all `CONT_EFFECT`
resumes multi-shot. This is the plan's explicitly-deferred
"`^multishot` cross-function resume" follow-up.

## Where this came from

`docs/upcoming/v1/cps-backend-n6-crossfn-resume-desugar-plan.md` (safety notes:
"Effect continuations are one-shot by default ... a single-resume shift is the
clean first target"; out of scope: "Multi-shot cross-function resume beyond
wiring `^multishot`").
